/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Virtual esp_lcd panel built on top of the SDL that is vendored in this
 * component.
 *
 * The panel is a regular esp_lcd panel that lives in the process of the
 * application: the SDL backend renders the framebuffer of the panel into a
 * window of the ESP-IDF host (POSIX) target, and it can export that framebuffer
 * as a PNG file, which is how a host test verifies the rendered frame.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <sys/param.h>
#include "esp_lcd_types.h"
#include "esp_lcd_panel_interface.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lcd_sim_panel.h"

static const char *TAG = "lcd_sim";

#define SIM_DEFAULT_SCREENSHOT_PATH "screenshot.png"

/*
 * esp_lcd_panel_t must be the first member so the object can be exposed as a
 * normal esp_lcd panel handle. Use __containerof() when recovering this private
 * object from the public base pointer.
 */
typedef struct {
    esp_lcd_panel_t base;
    int width;
    int height;
    size_t bytes_per_pixel;
    esp_color_fourcc_t color_format;
    uint8_t *framebuffer;
    size_t framebuffer_size;
    void *sdl_ctx;
    /* Owned copy of where the preview window saves a screenshot. The config
     * string may be a temporary, so the panel keeps its own copy. */
    char *screenshot_path;
    /* The event callbacks of the panel, copied by
     * esp_lcd_sim_register_event_callbacks(). Empty means no callbacks. */
    esp_lcd_sim_event_callbacks_t callbacks;
    void *user_ctx;
} sim_panel_t;

static esp_err_t sim_panel_del(esp_lcd_panel_t *panel);
static esp_err_t sim_panel_init(esp_lcd_panel_t *panel);
static esp_err_t sim_panel_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                        int x_end, int y_end, const void *color_data);
static esp_err_t sim_panel_draw_bitmap_2d(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                           const void *src_data, size_t src_x_size, size_t src_y_size,
                                           int src_x_start, int src_y_start, int src_x_end, int src_y_end);
static void sim_panel_screenshot(void *user_ctx);
static void sim_panel_flush_done(sim_panel_t *drv);

/* Screenshot key handler: write the on-screen frame (the panel framebuffer
 * itself) next to the configured file name. Consecutive presses never
 * overwrite: the configured name is used while it is free, then the first
 * free numbered variant ("shot_001.png", "shot_002.png", ...). Scanning the
 * directory instead of counting in memory keeps the numbering correct across
 * restarts. esp_lcd_sim_screenshot_save_png() keeps exact-path semantics for
 * programmatic exports. */
static void sim_panel_screenshot(void *user_ctx)
{
    esp_lcd_panel_handle_t panel = user_ctx;
    sim_panel_t *drv = __containerof(panel, sim_panel_t, base);
    const char *base = drv->screenshot_path;
    size_t base_len = strlen(base);
    /* Worst case name: the stem + "_999" + the extension + NUL. */
    char *path = malloc(base_len + sizeof("_999"));
    if (!path) {
        ESP_LOGE(TAG, "screenshot key: no memory for the file name");
        return;
    }

    /* The number goes before the extension, at the last '.' of the file name
     * itself: a dot in a directory ("dir.v2/shot") is not an extension. */
    const char *dot = strrchr(base, '.');
    const char *slash = strrchr(base, '/');
    if (dot && slash && dot < slash) {
        dot = NULL;
    }
    int stem_len = dot ? (int)(dot - base) : (int)base_len;
    const char *ext = dot ? dot : "";

    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (int n = 0; n < 1000; n++) {
        if (n == 0) {
            memcpy(path, base, base_len + 1);
        } else {
            snprintf(path, base_len + sizeof("_999"), "%.*s_%03d%s", stem_len, base, n, ext);
        }
        if (access(path, F_OK) != 0) {
            err = esp_lcd_sim_screenshot_save_png(panel, path);
            break;
        }
    }
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "screenshot key: '%s' and its 999 numbered variants all exist", base);
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "screenshot key: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "screenshot key: wrote '%s'", path);
    }
    free(path);
}

/* Report flush completion where a real transfer-based driver would report the
 * color transfer done. */
static void sim_panel_flush_done(sim_panel_t *drv)
{
    if (drv->callbacks.on_flush_done) {
        drv->callbacks.on_flush_done(&drv->base, drv->user_ctx);
    }
}

esp_err_t esp_lcd_sim_register_event_callbacks(esp_lcd_panel_handle_t panel,
                                               const esp_lcd_sim_event_callbacks_t *callbacks, void *user_ctx)
{
    ESP_RETURN_ON_FALSE(panel && panel->del == sim_panel_del, ESP_ERR_INVALID_ARG, TAG, "invalid panel handle");
    sim_panel_t *drv = __containerof(panel, sim_panel_t, base);
    if (callbacks) {
        drv->callbacks = *callbacks;
        drv->user_ctx = user_ctx;
    } else {
        memset(&drv->callbacks, 0, sizeof(drv->callbacks));
        drv->user_ctx = NULL;
    }
    return ESP_OK;
}

esp_err_t esp_lcd_new_panel_sim_sdl(const esp_lcd_sim_config_t *config, esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    sim_panel_t *sim_panel = NULL;
    size_t bytes_per_pixel = 0;
    uint32_t sdl_format = 0;

    ESP_RETURN_ON_FALSE(config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(config->width > 0 && config->height > 0, ESP_ERR_INVALID_ARG, TAG, "invalid panel size");
    ESP_RETURN_ON_FALSE(config->scale >= 0, ESP_ERR_INVALID_ARG, TAG, "invalid scale");
    int scale = config->scale > 0 ? config->scale : 1;
    ESP_RETURN_ON_FALSE((int64_t)config->width * scale <= INT_MAX && (int64_t)config->height * scale <= INT_MAX,
                        ESP_ERR_INVALID_ARG, TAG, "scaled window size overflow");
    ESP_RETURN_ON_ERROR(esp_lcd_sim_sdl_pixel_format(config->color_format, &sdl_format, &bytes_per_pixel),
                        TAG, "unsupported color format");
    ESP_RETURN_ON_FALSE((size_t)config->width <= SIZE_MAX / bytes_per_pixel
                        && (size_t)config->width * bytes_per_pixel <= SIZE_MAX / (size_t)config->height,
                        ESP_ERR_INVALID_ARG, TAG, "panel size overflow");

    sim_panel = calloc(1, sizeof(sim_panel_t));
    ESP_GOTO_ON_FALSE(sim_panel, ESP_ERR_NO_MEM, err, TAG, "no mem for sim panel");

    sim_panel->width = config->width;
    sim_panel->height = config->height;
    sim_panel->bytes_per_pixel = bytes_per_pixel;
    sim_panel->color_format = config->color_format;
    sim_panel->framebuffer_size = (size_t)config->width * config->height * bytes_per_pixel;
    /* calloc() keeps the first screenshot of a panel that is never drawn to
     * well defined instead of showing uninitialized heap. */
    sim_panel->framebuffer = calloc(1, sim_panel->framebuffer_size);
    ESP_GOTO_ON_FALSE(sim_panel->framebuffer, ESP_ERR_NO_MEM, err, TAG, "no mem for framebuffer");

    sim_panel->screenshot_path = strdup(config->screenshot_path ? config->screenshot_path
                                                                 : SIM_DEFAULT_SCREENSHOT_PATH);
    ESP_GOTO_ON_FALSE(sim_panel->screenshot_path, ESP_ERR_NO_MEM, err, TAG, "no mem for screenshot path");

    /* The preview window paints the panel framebuffer directly, so submitting a
     * frame copies nothing. The SDL pixel format was resolved above and is
     * handed to the backend, which has to re-validate nothing. The backend only
     * sees the key press; the panel owns the framebuffer and the encoder, so it
     * hands over the function to call for it. A panel without a preview window
     * has no backend and ignores the key anyway. */
    ESP_GOTO_ON_ERROR(esp_lcd_sim_sdl_backend_create(config, sim_panel->framebuffer,
                                                      sim_panel->framebuffer_size, sdl_format, bytes_per_pixel,
                                                      sim_panel_screenshot, &sim_panel->base,
                                                      &sim_panel->sdl_ctx),
                      err, TAG, "failed to create SDL backend");

    sim_panel->base.del = sim_panel_del;
    /* init() is mandatory in the esp_lcd panel interface (the generic wrapper
     * calls the callback without a NULL check), reset() is optional and stays
     * NULL: the generic esp_lcd wrappers report ESP_ERR_NOT_SUPPORTED for it
     * and the application can simply continue. */
    sim_panel->base.init = sim_panel_init;
    sim_panel->base.draw_bitmap = sim_panel_draw_bitmap;
    sim_panel->base.draw_bitmap_2d = sim_panel_draw_bitmap_2d;
    sim_panel->base.disp_on_off = NULL;
    sim_panel->base.invert_color = NULL;
    sim_panel->base.mirror = NULL;
    sim_panel->base.swap_xy = NULL;
    sim_panel->base.set_gap = NULL;

    *ret_panel = &sim_panel->base;
    ESP_LOGI(TAG, "SDL sim panel created (%dx%d, %s)", sim_panel->width, sim_panel->height,
             sim_panel->sdl_ctx ? "with preview window" : "framebuffer only");
    return ESP_OK;

err:
    if (sim_panel) {
        free(sim_panel->screenshot_path);
        free(sim_panel->framebuffer);
        free(sim_panel);
    }
    return ret;
}

esp_err_t esp_lcd_sim_pump_events(void)
{
    return esp_lcd_sim_sdl_backend_pump();
}

esp_err_t esp_lcd_sim_wait_events(uint32_t timeout_ms)
{
    return esp_lcd_sim_sdl_backend_wait_events(timeout_ms);
}

esp_err_t esp_lcd_sim_mouse_get(esp_lcd_panel_handle_t panel, esp_lcd_sim_mouse_data_t *data)
{
    ESP_RETURN_ON_FALSE(data, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    /* This is an entry point that takes a handle from outside the driver, so
     * identify handles owned by this driver before __containerof(). */
    ESP_RETURN_ON_FALSE(panel && panel->del == sim_panel_del, ESP_ERR_INVALID_ARG, TAG, "invalid panel handle");
    sim_panel_t *drv = __containerof(panel, sim_panel_t, base);
    return esp_lcd_sim_sdl_backend_mouse_get(drv->sdl_ctx, data);
}

bool esp_lcd_sim_window_is_open(esp_lcd_panel_handle_t panel)
{
    if (!panel || panel->del != sim_panel_del) {
        return false;
    }
    sim_panel_t *drv = __containerof(panel, sim_panel_t, base);
    return esp_lcd_sim_sdl_backend_window_is_open(drv->sdl_ctx);
}

esp_err_t esp_lcd_sim_deinit(void)
{
    return esp_lcd_sim_sdl_backend_deinit();
}

static esp_err_t sim_panel_del(esp_lcd_panel_t *panel)
{
    sim_panel_t *drv = __containerof(panel, sim_panel_t, base);
    esp_lcd_sim_sdl_backend_delete(drv->sdl_ctx);
    free(drv->screenshot_path);
    free(drv->framebuffer);
    free(drv);
    return ESP_OK;
}

static esp_err_t sim_panel_init(esp_lcd_panel_t *panel)
{
    sim_panel_t *drv = __containerof(panel, sim_panel_t, base);
    /* Power-on state of the simulated panel: every pixel off (a zeroed
     * framebuffer is black in all supported color formats). */
    memset(drv->framebuffer, 0, drv->framebuffer_size);
    /* Show the power-on frame even before the GUI draws anything. */
    return esp_lcd_sim_sdl_backend_update(drv->sdl_ctx, drv->framebuffer, drv->framebuffer_size);
}

/* Copy a source crop into the panel framebuffer. The source crop and target
 * rectangle must have the same dimensions, this driver does not scale. */
static esp_err_t sim_panel_copy_bitmap(sim_panel_t *drv,
                                        int x_start, int y_start, int x_end, int y_end,
                                        const void *src_data, size_t src_x_size, size_t src_y_size,
                                        int src_x_start, int src_y_start, int src_x_end, int src_y_end)
{
    int64_t target_width = (int64_t)x_end - x_start;
    int64_t target_height = (int64_t)y_end - y_start;
    int64_t source_width = (int64_t)src_x_end - src_x_start;
    int64_t source_height = (int64_t)src_y_end - src_y_start;

    /* The public esp_lcd wrapper validates target/source ordering. The
     * remaining checks protect this driver's source buffer calculations. */
    ESP_RETURN_ON_FALSE(src_data
                        && source_width == target_width && source_height == target_height
                        && src_x_start >= 0 && src_y_start >= 0
                        && (uint64_t)src_x_end <= src_x_size && (uint64_t)src_y_end <= src_y_size
                        && src_x_size <= SIZE_MAX / drv->bytes_per_pixel
                        && src_x_size > 0
                        && src_y_size <= SIZE_MAX / src_x_size,
                        ESP_ERR_INVALID_ARG, TAG, "invalid bitmap region");

    /* Clip the target to the panel framebuffer and shift the source crop by
     * exactly the same amount so source and destination stay aligned. */
    int clipped_x_start = MAX(x_start, 0);
    int clipped_y_start = MAX(y_start, 0);
    int clipped_x_end = MIN(x_end, drv->width);
    int clipped_y_end = MIN(y_end, drv->height);
    int64_t clipped_src_x_start = (int64_t)src_x_start + clipped_x_start - x_start;
    int64_t clipped_src_y_start = (int64_t)src_y_start + clipped_y_start - y_start;

    if (clipped_x_start < clipped_x_end && clipped_y_start < clipped_y_end) {
        const uint8_t *src = (const uint8_t *)src_data
                             + ((size_t)clipped_src_y_start * src_x_size + (size_t)clipped_src_x_start) * drv->bytes_per_pixel;
        uint8_t *dst = drv->framebuffer
                       + ((size_t)clipped_y_start * drv->width + clipped_x_start) * drv->bytes_per_pixel;
        size_t copy_bytes = (size_t)(clipped_x_end - clipped_x_start) * drv->bytes_per_pixel;
        size_t src_stride = src_x_size * drv->bytes_per_pixel;
        size_t dst_stride = (size_t)drv->width * drv->bytes_per_pixel;

        if (copy_bytes == src_stride && copy_bytes == dst_stride) {
            /* The region spans the full width of both buffers, so its rows
             * are contiguous in each: one copy moves the whole region. */
            memcpy(dst, src, copy_bytes * (size_t)(clipped_y_end - clipped_y_start));
        } else {
            for (int y = clipped_y_start; y < clipped_y_end; y++) {
                memcpy(dst, src, copy_bytes);
                src += src_stride;
                dst += dst_stride;
            }
        }
    }
    return ESP_OK;
}

static esp_err_t sim_panel_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                        int x_end, int y_end, const void *color_data)
{
    sim_panel_t *drv = __containerof(panel, sim_panel_t, base);
    ESP_RETURN_ON_ERROR(sim_panel_copy_bitmap(drv, x_start, y_start, x_end, y_end, color_data,
                                               (size_t)(x_end - x_start), (size_t)(y_end - y_start),
                                               0, 0, x_end - x_start, y_end - y_start),
                        TAG, "invalid bitmap region");
    /* The preview window shares the framebuffer, mark only the drawn region
     * dirty so the next esp_lcd_sim_pump_events() repaints what changed. */
    esp_err_t ret = esp_lcd_sim_sdl_backend_update_rect(drv->sdl_ctx, drv->framebuffer, drv->framebuffer_size,
                                                        x_start, y_start, x_end - x_start, y_end - y_start);
    if (ret == ESP_OK) {
        sim_panel_flush_done(drv);
    }
    return ret;
}

static esp_err_t sim_panel_draw_bitmap_2d(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                           const void *src_data, size_t src_x_size, size_t src_y_size,
                                           int src_x_start, int src_y_start, int src_x_end, int src_y_end)
{
    sim_panel_t *drv = __containerof(panel, sim_panel_t, base);
    ESP_RETURN_ON_ERROR(sim_panel_copy_bitmap(drv, x_start, y_start, x_end, y_end, src_data,
                                               src_x_size, src_y_size, src_x_start, src_y_start,
                                               src_x_end, src_y_end),
                        TAG, "invalid bitmap region");
    esp_err_t ret = esp_lcd_sim_sdl_backend_update_rect(drv->sdl_ctx, drv->framebuffer, drv->framebuffer_size,
                                                        x_start, y_start, x_end - x_start, y_end - y_start);
    if (ret == ESP_OK) {
        sim_panel_flush_done(drv);
    }
    return ret;
}

esp_err_t esp_lcd_sim_panel_get_info(esp_lcd_panel_handle_t panel, esp_lcd_sim_panel_info_t *info)
{
    ESP_RETURN_ON_FALSE(info, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    /* This is the one entry point that takes a handle from outside the driver,
     * so identify handles owned by this driver before __containerof(). */
    ESP_RETURN_ON_FALSE(panel && panel->del == sim_panel_del, ESP_ERR_INVALID_ARG, TAG, "invalid panel handle");
    sim_panel_t *drv = __containerof(panel, sim_panel_t, base);

    info->width = drv->width;
    info->height = drv->height;
    info->bytes_per_pixel = drv->bytes_per_pixel;
    info->color_format = drv->color_format;
    info->framebuffer = drv->framebuffer;
    info->framebuffer_size = drv->framebuffer_size;
    return ESP_OK;
}
