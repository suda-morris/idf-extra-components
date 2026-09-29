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
#include <stdlib.h>
#include <string.h>
#include "esp_lcd_host_types.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lcd_host_panel.h"

static const char *TAG = "lcd_host";

/* The host target is built for Linux, macOS and Windows, and the MIN/MAX macros
 * that <sys/param.h> provides on the C libraries of the first two are not part
 * of the C standard. Define them here instead of relying on a header that MSVC
 * does not have. */
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

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
} host_panel_t;

static esp_err_t host_panel_del(esp_lcd_panel_t *panel);
static esp_err_t host_panel_init(esp_lcd_panel_t *panel);
static esp_err_t host_panel_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                        int x_end, int y_end, const void *color_data);
static esp_err_t host_panel_draw_bitmap_2d(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                           const void *src_data, size_t src_x_size, size_t src_y_size,
                                           int src_x_start, int src_y_start, int src_x_end, int src_y_end);

/* Recover the private panel object from a public handle. The destructor
 * identifies handles owned by this driver before __containerof() is applied. */
static esp_err_t host_panel_check(esp_lcd_panel_handle_t panel, host_panel_t **ret_drv)
{
    ESP_RETURN_ON_FALSE(panel && panel->del == host_panel_del, ESP_ERR_INVALID_ARG, TAG, "invalid panel handle");
    *ret_drv = __containerof(panel, host_panel_t, base);
    return ESP_OK;
}

esp_err_t esp_lcd_new_panel_host_sdl(const esp_lcd_host_config_t *config, esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    host_panel_t *host_panel = NULL;
    size_t bytes_per_pixel = 0;
    uint32_t sdl_format = 0;

    ESP_RETURN_ON_FALSE(config && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(config->width > 0 && config->height > 0, ESP_ERR_INVALID_ARG, TAG, "invalid panel size");
    ESP_RETURN_ON_ERROR(esp_lcd_host_sdl_pixel_format(config->color_format, &sdl_format, &bytes_per_pixel),
                        TAG, "unsupported color format");
    ESP_RETURN_ON_FALSE((size_t)config->width <= SIZE_MAX / bytes_per_pixel
                        && (size_t)config->width * bytes_per_pixel <= SIZE_MAX / (size_t)config->height,
                        ESP_ERR_INVALID_ARG, TAG, "panel size overflow");

    host_panel = calloc(1, sizeof(host_panel_t));
    ESP_GOTO_ON_FALSE(host_panel, ESP_ERR_NO_MEM, err, TAG, "no mem for host panel");

    host_panel->width = config->width;
    host_panel->height = config->height;
    host_panel->bytes_per_pixel = bytes_per_pixel;
    host_panel->color_format = config->color_format;
    host_panel->framebuffer_size = (size_t)config->width * config->height * bytes_per_pixel;
    /* calloc() keeps the first screenshot of a panel that is never drawn to
     * well defined instead of showing uninitialized heap. */
    host_panel->framebuffer = calloc(1, host_panel->framebuffer_size);
    ESP_GOTO_ON_FALSE(host_panel->framebuffer, ESP_ERR_NO_MEM, err, TAG, "no mem for framebuffer");

    /* The preview window paints the panel framebuffer directly, so submitting a
     * frame copies nothing. */
    ESP_GOTO_ON_ERROR(esp_lcd_host_sdl_backend_create(config, host_panel->framebuffer,
                                                      host_panel->framebuffer_size, &host_panel->sdl_ctx),
                      err, TAG, "failed to create SDL backend");

    host_panel->base.del = host_panel_del;
    /* init() is mandatory in the esp_lcd panel interface (the generic wrapper
     * calls the callback without a NULL check), reset() is optional and stays
     * NULL: the generic esp_lcd wrappers report ESP_ERR_NOT_SUPPORTED for it
     * and the application can simply continue. */
    host_panel->base.reset = NULL;
    host_panel->base.init = host_panel_init;
    host_panel->base.draw_bitmap = host_panel_draw_bitmap;
    host_panel->base.draw_bitmap_2d = host_panel_draw_bitmap_2d;
    host_panel->base.disp_on_off = NULL;
    host_panel->base.invert_color = NULL;
    host_panel->base.mirror = NULL;
    host_panel->base.swap_xy = NULL;
    host_panel->base.set_gap = NULL;

    *ret_panel = &host_panel->base;
    ESP_LOGI(TAG, "Host SDL panel created (%dx%d, %s)", host_panel->width, host_panel->height,
             config->create_window ? "with preview window" : "framebuffer only");
    return ESP_OK;

err:
    if (host_panel) {
        free(host_panel->framebuffer);
        free(host_panel);
    }
    return ret;
}

esp_err_t esp_lcd_host_get_target(esp_lcd_panel_handle_t panel, esp_lcd_host_target_t *target)
{
    host_panel_t *drv = NULL;
    ESP_RETURN_ON_FALSE(target, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_ERROR(host_panel_check(panel, &drv), TAG, "invalid panel handle");

    /* The component only builds for the host target. */
    *target = ESP_LCD_HOST_TARGET_POSIX;
    return ESP_OK;
}

esp_err_t esp_lcd_host_return_panel(esp_lcd_panel_handle_t panel)
{
    host_panel_t *drv = NULL;
    ESP_RETURN_ON_ERROR(host_panel_check(panel, &drv), TAG, "invalid panel handle");
    /* The SDL backend renders the framebuffer of the panel, which it shares
     * with the panel, so there is nothing else to publish. The call is kept so
     * a GUI port that publishes its frame can use one code path. */
    return ESP_OK;
}

esp_err_t esp_lcd_host_return_buffers(esp_lcd_panel_handle_t panel)
{
    host_panel_t *drv = NULL;
    ESP_RETURN_ON_ERROR(host_panel_check(panel, &drv), TAG, "invalid panel handle");
    /* See esp_lcd_host_return_panel(): the framebuffer is already shared with
     * the SDL backend. */
    return ESP_OK;
}

esp_err_t esp_lcd_host_pump_events(void)
{
    return esp_lcd_host_sdl_backend_pump(NULL);
}

static esp_err_t host_panel_del(esp_lcd_panel_t *panel)
{
    host_panel_t *drv = __containerof(panel, host_panel_t, base);
    esp_lcd_host_sdl_backend_delete(drv->sdl_ctx);
    free(drv->framebuffer);
    free(drv);
    return ESP_OK;
}

static esp_err_t host_panel_init(esp_lcd_panel_t *panel)
{
    host_panel_t *drv = __containerof(panel, host_panel_t, base);
    /* Power-on state of the simulated panel: every pixel off (a zeroed
     * framebuffer is black in all supported color formats). */
    memset(drv->framebuffer, 0, drv->framebuffer_size);
    /* Show the power-on frame even before the GUI draws anything. */
    return esp_lcd_host_sdl_backend_update(drv->sdl_ctx, drv->framebuffer, drv->framebuffer_size);
}

/* Copy a source crop into the panel framebuffer. The source crop and target
 * rectangle must have the same dimensions, this driver does not scale. */
static esp_err_t host_panel_copy_bitmap(host_panel_t *drv,
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

        for (int y = clipped_y_start; y < clipped_y_end; y++) {
            memcpy(dst, src, copy_bytes);
            src += src_stride;
            dst += dst_stride;
        }
    }
    return ESP_OK;
}

static esp_err_t host_panel_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                        int x_end, int y_end, const void *color_data)
{
    host_panel_t *drv = __containerof(panel, host_panel_t, base);
    size_t src_x_size = (size_t)((int64_t)x_end - x_start);
    size_t src_y_size = (size_t)((int64_t)y_end - y_start);
    ESP_RETURN_ON_ERROR(host_panel_copy_bitmap(drv, x_start, y_start, x_end, y_end, color_data,
                                               src_x_size, src_y_size, 0, 0, (int)src_x_size, (int)src_y_size),
                        TAG, "invalid bitmap region");
    /* The preview window shares the framebuffer, mark only the drawn region
     * dirty so the next esp_lcd_host_pump_events() repaints what changed. */
    return esp_lcd_host_sdl_backend_update_rect(drv->sdl_ctx, drv->framebuffer, drv->framebuffer_size,
                                                x_start, y_start, (int)src_x_size, (int)src_y_size);
}

static esp_err_t host_panel_draw_bitmap_2d(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                           const void *src_data, size_t src_x_size, size_t src_y_size,
                                           int src_x_start, int src_y_start, int src_x_end, int src_y_end)
{
    host_panel_t *drv = __containerof(panel, host_panel_t, base);
    ESP_RETURN_ON_ERROR(host_panel_copy_bitmap(drv, x_start, y_start, x_end, y_end, src_data,
                                               src_x_size, src_y_size, src_x_start, src_y_start,
                                               src_x_end, src_y_end),
                        TAG, "invalid bitmap region");
    return esp_lcd_host_sdl_backend_update_rect(drv->sdl_ctx, drv->framebuffer, drv->framebuffer_size,
                                                x_start, y_start, x_end - x_start, y_end - y_start);
}

esp_err_t esp_lcd_host_panel_get_info(esp_lcd_panel_handle_t panel, esp_lcd_host_panel_info_t *info)
{
    host_panel_t *drv = NULL;
    ESP_RETURN_ON_FALSE(info, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_ERROR(host_panel_check(panel, &drv), TAG, "invalid panel handle");

    info->width = drv->width;
    info->height = drv->height;
    info->bytes_per_pixel = drv->bytes_per_pixel;
    info->color_format = drv->color_format;
    info->framebuffer = drv->framebuffer;
    info->framebuffer_size = drv->framebuffer_size;
    return ESP_OK;
}
