/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SDL backend for the esp_lcd_host panel driver.
 *
 * SDL keeps the panel content in the SDL surface of the preview window, whose
 * pixel buffer is the panel framebuffer itself, so submitting a frame copies
 * nothing. The window surface is refreshed lazily, otherwise the synchronous
 * SDL_UpdateWindowSurface() would steal time from the GUI.
 */
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "esp_check.h"
#include "esp_log.h"
#include "SDL3/SDL.h"
#include "esp_lcd_host_panel.h"

static const char *TAG = "lcd_host.sdl";

#define HOST_DEFAULT_WINDOW_TITLE "esp_lcd_host"

typedef struct {
    SDL_Window *window;
    SDL_Surface *surface;
    uint8_t *framebuffer;
    size_t framebuffer_size;
    size_t bytes_per_pixel;
    bool dirty;
} host_sdl_ctx_t;

esp_err_t esp_lcd_host_sdl_pixel_format(esp_color_fourcc_t fourcc, uint32_t *ret_format, size_t *ret_bytes)
{
    uint32_t format = 0;
    size_t bytes = 0;

    switch (fourcc) {
    /* SDL_PIXELFORMAT_RGB565 stores the value in native byte order, which is
     * what the little endian ESP_COLOR_FOURCC_RGB16 describes. */
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
        /* Framebuffer only: the content can still be exported as PNG or
         * returned to an external simulator, no SDL object needed. */
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_lcd_host_sdl_pixel_format(config->color_format, &sdl_format, &bytes_per_pixel),
                        TAG, "unsupported color format");

    ctx = calloc(1, sizeof(host_sdl_ctx_t));
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_NO_MEM, TAG, "no mem for SDL backend");
    ctx->framebuffer = framebuffer;
    ctx->framebuffer_size = framebuffer_size;
    ctx->bytes_per_pixel = bytes_per_pixel;

    /* Keep windows resizable so the preview can be zoomed by the user as well. */
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
    /* Without a logical presentation SDL expects the caller to match the window
     * size exactly, which would break user resizing. */
    if (!SDL_SetWindowAspectRatio(ctx->window, (float)config->width / (float)config->height,
                                  (float)config->width / (float)config->height)) {
        ESP_LOGW(TAG, "failed to set window aspect ratio: %s", SDL_GetError());
    }

    /* The surface wraps the panel framebuffer: drawing into it writes into the
     * framebuffer and no copy is needed when a frame is submitted. */
    ctx->surface = SDL_CreateSurfaceFrom(config->width, config->height, sdl_format, framebuffer,
                                         config->width * (int)bytes_per_pixel);
    if (!ctx->surface) {
        ESP_LOGE(TAG, "failed to create window surface: %s", SDL_GetError());
        SDL_DestroyWindow(ctx->window);
        free(ctx);
        return ESP_FAIL;
    }

    if (!SDL_SetWindowSurfaceVSync(ctx->window, 0)) {
        ESP_LOGW(TAG, "failed to disable preview vsync: %s", SDL_GetError());
    }

    ESP_LOGI(TAG, "preview window '%s' created (%dx%d, scale %d)",
             config->window_title ? config->window_title : HOST_DEFAULT_WINDOW_TITLE,
             config->width * scale, config->height * scale, scale);
    *ret_ctx = ctx;
    return ESP_OK;
}

void esp_lcd_host_sdl_backend_delete(void *ctx)
{
    host_sdl_ctx_t *sdl_ctx = ctx;
    if (!sdl_ctx) {
        return;
    }
    /* The surface wraps the panel framebuffer, which the panel owns and frees. */
    SDL_DestroySurface(sdl_ctx->surface);
    SDL_DestroyWindow(sdl_ctx->window);
    free(sdl_ctx);
}

esp_err_t esp_lcd_host_sdl_backend_update(void *ctx, const uint8_t *framebuffer, size_t framebuffer_size)
{
    host_sdl_ctx_t *sdl_ctx = ctx;
    if (!sdl_ctx) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(framebuffer == sdl_ctx->framebuffer && framebuffer_size == sdl_ctx->framebuffer_size,
                        ESP_ERR_INVALID_ARG, TAG, "framebuffer mismatch");
    sdl_ctx->dirty = true;
    return ESP_OK;
}

esp_err_t esp_lcd_host_sdl_backend_pump(void *ctx)
{
    host_sdl_ctx_t *sdl_ctx = ctx;
    /* esp_lcd_host_pump_events() drives every preview window of the process, so
     * it can also be called before the first panel exists. */
    if (sdl_ctx && sdl_ctx->dirty) {
        if (!SDL_UpdateWindowSurface(sdl_ctx->window)) {
            ESP_LOGE(TAG, "failed to update preview window: %s", SDL_GetError());
            return ESP_FAIL;
        }
        sdl_ctx->dirty = false;
    }
    SDL_PumpEvents();
    return ESP_OK;
}
