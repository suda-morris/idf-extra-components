/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SDL backend for the esp_lcd_host panel driver.
 *
 * Every panel framebuffer is uploaded into an SDL streaming texture and
 * presented through a renderer, which is the one output path SDL3 supports on
 * every platform of the host target (the window surface API has no
 * implementation in the Wayland driver). The texture upload copies the frame
 * once per presented frame; the panel framebuffer stays the single source of
 * truth that the GUI renders into and that the screenshot encoder reads.
 *
 * The presentation uses a logical size equal to the panel resolution, so SDL
 * scales the panel to whatever size the user made the preview window.
 *
 * The SDL headers come from esp_lcd_host/SDL, the submodule this component
 * builds through port/sdl/CMakeLists.txt.
 */
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>
#include "esp_check.h"
#include "esp_log.h"
#include "SDL3/SDL.h"
#include "esp_lcd_host_panel.h"

static const char *TAG = "lcd_host.sdl";

#define HOST_DEFAULT_WINDOW_TITLE "esp_lcd_host"

/* Set when the user closed a preview window (or quit the application from the
 * window manager). Sticky on purpose: an application that polls it once per
 * frame with several panels open still sees the request. */
static bool s_window_close_requested = false;

/* Registry of the SDL backends of the process. esp_lcd_host_pump_events() has
 * no panel handle, so it walks this list to present every dirty window and to
 * drain the shared SDL event queue. */
typedef struct host_sdl_ctx {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    uint8_t *framebuffer;
    size_t framebuffer_size;
    int width;
    int height;
    size_t bytes_per_pixel;
    bool dirty;
    struct host_sdl_ctx *next;
} host_sdl_ctx_t;

static host_sdl_ctx_t *s_backends = NULL;
static pthread_mutex_t s_backends_lock = PTHREAD_MUTEX_INITIALIZER;

esp_err_t esp_lcd_host_sdl_pixel_format(esp_color_fourcc_t fourcc, uint32_t *ret_format, size_t *ret_bytes)
{
    uint32_t format = 0;
    size_t bytes = 0;

    switch (fourcc) {
    /* A native endian RGB565 value is what ESP_COLOR_FOURCC_RGB16 describes on
     * the little endian host target, and SDL_PIXELFORMAT_RGB565 stores the
     * value in native byte order. */
    case ESP_COLOR_FOURCC_RGB16:
    case ESP_COLOR_FOURCC_RGB16_BE:
        format = SDL_PIXELFORMAT_RGB565;
        bytes = 2;
        break;
    case ESP_COLOR_FOURCC_RGB24:
        format = SDL_PIXELFORMAT_RGB24;
        bytes = 3;
        break;
    case ESP_COLOR_FOURCC_BGR24:
        format = SDL_PIXELFORMAT_BGR24;
        bytes = 3;
        break;
    case ESP_COLOR_FOURCC_BGRA32:
        format = SDL_PIXELFORMAT_BGRA32;
        bytes = 4;
        break;
    default:
        ESP_LOGE(TAG, "unsupported color format 0x%" PRIx32, (uint32_t)fourcc);
        return ESP_ERR_NOT_SUPPORTED;
    }

    *ret_format = format;
    *ret_bytes = bytes;
    return ESP_OK;
}

esp_err_t esp_lcd_host_sdl_backend_create(const esp_lcd_host_config_t *config, uint8_t *framebuffer,
                                          size_t framebuffer_size, void **ret_ctx)
{
    host_sdl_ctx_t *ctx = NULL;
    uint32_t sdl_format = 0;
    size_t bytes_per_pixel = 0;
    int scale = config->scale > 0 ? config->scale : 1;

    *ret_ctx = NULL;
    if (!config->create_window) {
        /* Framebuffer only: the content can still be exported as a PNG file,
         * no SDL object needed. */
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_lcd_host_sdl_pixel_format(config->color_format, &sdl_format, &bytes_per_pixel),
                        TAG, "unsupported color format");

    ctx = calloc(1, sizeof(host_sdl_ctx_t));
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_NO_MEM, TAG, "no mem for SDL backend");
    ctx->framebuffer = framebuffer;
    ctx->framebuffer_size = framebuffer_size;
    ctx->bytes_per_pixel = bytes_per_pixel;
    ctx->width = config->width;
    ctx->height = config->height;

    /* Keep windows resizable so the preview can be zoomed by the user as well.
     * The initial size is the panel size times the requested scale. */
    ctx->window = SDL_CreateWindow(config->window_title ? config->window_title : HOST_DEFAULT_WINDOW_TITLE,
                                   config->width * scale, config->height * scale, SDL_WINDOW_RESIZABLE);
    if (!ctx->window) {
        ESP_LOGE(TAG, "failed to create preview window: %s", SDL_GetError());
        free(ctx);
        return ESP_FAIL;
    }
    if (!SDL_SetWindowMinimumSize(ctx->window, config->width, config->height)) {
        ESP_LOGW(TAG, "failed to set window minimum size: %s", SDL_GetError());
    }
    /* Keep the window shape close to the panel shape while the user resizes.
     * SDL scales the panel into the window with the logical presentation set
     * up below, so the pixels stay square. */
    if (!SDL_SetWindowAspectRatio(ctx->window, (float)config->width / (float)config->height,
                                  (float)config->width / (float)config->height)) {
        ESP_LOGW(TAG, "failed to set window aspect ratio: %s", SDL_GetError());
    }

    ctx->renderer = SDL_CreateRenderer(ctx->window, NULL);
    if (!ctx->renderer) {
        ESP_LOGE(TAG, "failed to create preview renderer: %s", SDL_GetError());
        SDL_DestroyWindow(ctx->window);
        free(ctx);
        return ESP_FAIL;
    }
    /* The whole panel is the logical scene: SDL scales it to the window, and
     * SDL_RenderPresent() of a dirty frame is all the preview needs. */
    if (!SDL_SetRenderLogicalPresentation(ctx->renderer, config->width, config->height,
                                          SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
        ESP_LOGW(TAG, "failed to set the preview scaling: %s", SDL_GetError());
    }

    ctx->texture = SDL_CreateTexture(ctx->renderer, sdl_format, SDL_TEXTUREACCESS_STREAMING,
                                     config->width, config->height);
    if (!ctx->texture) {
        ESP_LOGE(TAG, "failed to create preview texture: %s", SDL_GetError());
        SDL_DestroyRenderer(ctx->renderer);
        SDL_DestroyWindow(ctx->window);
        free(ctx);
        return ESP_FAIL;
    }

    pthread_mutex_lock(&s_backends_lock);
    ctx->next = s_backends;
    s_backends = ctx;
    pthread_mutex_unlock(&s_backends_lock);

    ESP_LOGI(TAG, "preview window '%s' created (%dx%d, scale %d)",
             config->window_title ? config->window_title : HOST_DEFAULT_WINDOW_TITLE,
             config->width * scale, config->height * scale, scale);
    *ret_ctx = ctx;
    return ESP_OK;
}

/* Drop every SDL resource of one backend. Called with the lock held when the
 * backend is still on the registry, and without it from the delete path after
 * the backend was already removed. */
static void host_sdl_free_objects(host_sdl_ctx_t *ctx)
{
    if (ctx->texture) {
        SDL_DestroyTexture(ctx->texture);
        ctx->texture = NULL;
    }
    if (ctx->renderer) {
        SDL_DestroyRenderer(ctx->renderer);
        ctx->renderer = NULL;
    }
    if (ctx->window) {
        SDL_DestroyWindow(ctx->window);
        ctx->window = NULL;
    }
    ctx->dirty = false;
}

/* Close the window of one backend after a close request, keeping the rest of
 * the backend alive: the panel framebuffer still renders and exports PNGs. */
static void host_sdl_close_backend(host_sdl_ctx_t *ctx)
{
    pthread_mutex_lock(&s_backends_lock);
    host_sdl_free_objects(ctx);
    pthread_mutex_unlock(&s_backends_lock);
    ESP_LOGI(TAG, "preview window closed by the user");
}

void esp_lcd_host_sdl_backend_delete(void *backend)
{
    host_sdl_ctx_t *ctx = backend;
    if (!ctx) {
        return;
    }
    pthread_mutex_lock(&s_backends_lock);
    host_sdl_ctx_t **link = &s_backends;
    while (*link && *link != ctx) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = ctx->next;
    }
    host_sdl_free_objects(ctx);
    pthread_mutex_unlock(&s_backends_lock);
    free(ctx);
}

esp_err_t esp_lcd_host_sdl_backend_update(void *backend, const uint8_t *framebuffer, size_t framebuffer_size)
{
    host_sdl_ctx_t *ctx = backend;
    if (!ctx) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(framebuffer == ctx->framebuffer && framebuffer_size == ctx->framebuffer_size,
                        ESP_ERR_INVALID_ARG, TAG, "framebuffer mismatch");
    ctx->dirty = true;
    return ESP_OK;
}

esp_err_t esp_lcd_host_sdl_backend_pump(void *backend)
{
    /* esp_lcd_host_pump_events() has no panel handle and walks the registry, so
     * it can also be called before the first panel exists. */
    (void) backend;

    /* The panel drives the preview windows, so the shared SDL event queue would
     * only pile up unread events. Poll it here and watch for windows being
     * closed: SDL delivers a close request instead of destroying the window,
     * and an unread request would leave a dead window on screen. */
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
            s_window_close_requested = true;
        } else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            SDL_Window *closed = SDL_GetWindowFromID(event.window.windowID);
            host_sdl_ctx_t *owner = NULL;
            pthread_mutex_lock(&s_backends_lock);
            for (host_sdl_ctx_t *ctx = s_backends; ctx; ctx = ctx->next) {
                if (ctx->window == closed) {
                    owner = ctx;
                    break;
                }
            }
            pthread_mutex_unlock(&s_backends_lock);
            if (owner) {
                host_sdl_close_backend(owner);
                s_window_close_requested = true;
            }
        }
        /* Anything else (input events and the like) is dropped: the simulator
         * has no input API yet, and no application sees this queue. */
    }

    /* Present every dirty window. SDL_RenderPresent() is vsynced where the
     * driver supports it, which throttles the redraws of an idle GUI for free;
     * headless builds have no window and nothing to present. */
    pthread_mutex_lock(&s_backends_lock);
    for (host_sdl_ctx_t *ctx = s_backends; ctx; ctx = ctx->next) {
        if (!ctx->dirty || !ctx->renderer) {
            continue;
        }
        ctx->dirty = false;
        if (!SDL_UpdateTexture(ctx->texture, NULL, ctx->framebuffer,
                               (int)(ctx->width * ctx->bytes_per_pixel))) {
            ESP_LOGE(TAG, "failed to upload the frame: %s", SDL_GetError());
            continue;
        }
        if (!SDL_RenderClear(ctx->renderer) ||
            !SDL_RenderTexture(ctx->renderer, ctx->texture, NULL, NULL) ||
            !SDL_RenderPresent(ctx->renderer)) {
            ESP_LOGE(TAG, "failed to present the frame: %s", SDL_GetError());
        }
    }
    pthread_mutex_unlock(&s_backends_lock);
    return ESP_OK;
}

bool esp_lcd_host_window_close_requested(void)
{
    return s_window_close_requested;
}
