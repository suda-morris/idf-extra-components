/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_lcd_host.h"
#include "esp_lcd_types.h"
#include "esp_lcd_panel_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Backends driving an esp_lcd_host panel.
 *
 * The panel object lives in esp_lcd_host.c and issues every backend call from
 * there, so a future panel implementation can be built on another backend
 * without the panel object having to know about it.
 *
 * All backend functions, like the public API of the component, must be called
 * from a single thread: SDL windows and its event queue are not thread-safe,
 * so this component does not add any locking of its own.
 */

/**
 * @brief Create the SDL objects backing one panel
 *
 * The backend uploads the framebuffer of the panel into a texture, so the
 * panel itself never talks to SDL directly and stays independent of the SDL
 * version in use.
 *
 * @param[in]  config        Panel configuration, read by the backend
 * @param[in]  framebuffer   Panel framebuffer, source of the preview uploads
 * @param[in]  framebuffer_size Size of the framebuffer in bytes
 * @param[in]  sdl_format    SDL pixel format matching the framebuffer layout,
 *                           as returned by esp_lcd_host_sdl_pixel_format()
 * @param[in]  bytes_per_pixel Bytes per pixel of the framebuffer format
 * @param[out] ret_ctx       Returned backend context, NULL if the backend has no state
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_NO_MEM if a SDL resource or the internal buffer cannot be allocated
 *      - ESP_FAIL if SDL cannot create the window, renderer or texture
 */
esp_err_t esp_lcd_host_sdl_backend_create(const esp_lcd_host_config_t *config, uint8_t *framebuffer,
                                          size_t framebuffer_size, uint32_t sdl_format, size_t bytes_per_pixel,
                                          void **ret_ctx);

/**
 * @brief Called when the user asks for a screenshot from a preview window
 *
 * The backend only sees SDL events, while the panel owns the framebuffer and
 * the encoder, so the panel installs this and the backend calls it.
 *
 * @param[in] user_ctx  Context given to esp_lcd_host_sdl_backend_set_screenshot_fn()
 */
typedef void (*esp_lcd_host_screenshot_fn_t)(void *user_ctx);

/**
 * @brief Set the function the backend calls when the screenshot key is pressed
 *
 * Called by esp_lcd_new_panel_host_sdl() with a function that writes the PNG.
 * An application that wants to handle the key itself leaves `fn` NULL and
 * calls esp_lcd_host_screenshot_save_png() whenever it likes.
 *
 * @param[in] backend     Backend context, NULL is accepted and ignored
 * @param[in] fn          Function to call, NULL disables the key
 * @param[in] user_ctx    Passed to `fn` on every press
 */
esp_err_t esp_lcd_host_sdl_backend_set_screenshot_fn(void *backend,
                                                     esp_lcd_host_screenshot_fn_t fn,
                                                     void *user_ctx);

/**
 * @brief Release one SDL backend, called from esp_lcd_panel_del()
 */
void esp_lcd_host_sdl_backend_delete(void *ctx);

/**
 * @brief Push the latest panel content to the preview
 *
 * Marks the whole panel dirty: the next esp_lcd_host_pump_events() uploads the
 * complete framebuffer into the preview texture and presents it.
 */
esp_err_t esp_lcd_host_sdl_backend_update(void *ctx, const uint8_t *framebuffer, size_t framebuffer_size);

/**
 * @brief Push a panel region to the preview
 *
 * Marks the region dirty and unions it with the regions of the not yet
 * presented flushes, so a GUI that flushes in partial areas uploads only what
 * changed on the next pump call.
 *
 * @param[in] x      Left edge of the region in panel pixels
 * @param[in] y      Top edge of the region in panel pixels
 * @param[in] width  Width of the region in pixels
 * @param[in] height Height of the region in pixels
 */
esp_err_t esp_lcd_host_sdl_backend_update_rect(void *ctx, const uint8_t *framebuffer, size_t framebuffer_size,
                                               int x, int y, int width, int height);

/**
 * @brief Pump the SDL event queue, called from esp_lcd_host_pump_events()
 */
esp_err_t esp_lcd_host_sdl_backend_pump(void);

/**
 * @brief Release the global SDL state, called from esp_lcd_host_deinit()
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if a live backend remains
 */
esp_err_t esp_lcd_host_sdl_backend_deinit(void);

/**
 * @brief Convert a fourcc to the SDL pixel format and byte count for it
 *
 * Both RGB565 flavors map to the native endian SDL_PIXELFORMAT_RGB565; the
 * backend byte-swaps the flavors whose memory layout differs from the host
 * endianness during the upload.
 *
 * @param[in]  fourcc        Framebuffer color format
 * @param[out] ret_format    Returned SDL pixel format
 * @param[out] ret_bytes     Returned bytes per pixel
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED for an unknown fourcc
 */
esp_err_t esp_lcd_host_sdl_pixel_format(esp_color_fourcc_t fourcc, uint32_t *ret_format, size_t *ret_bytes);

/**
 * @brief Snapshot of the state of an esp_lcd_host panel
 *
 * Used by the screenshot helpers, which live in a separate translation unit and
 * therefore cannot read the private panel object directly.
 */
typedef struct {
    int width;                      /*!< Panel width in pixels */
    int height;                     /*!< Panel height in pixels */
    size_t bytes_per_pixel;         /*!< Bytes per pixel of `color_format` */
    esp_color_fourcc_t color_format;/*!< Framebuffer color format */
    uint8_t *framebuffer;           /*!< Panel framebuffer, owned by the panel */
    size_t framebuffer_size;        /*!< Size of the framebuffer in bytes */
} esp_lcd_host_panel_info_t;

/**
 * @brief Read the state of an esp_lcd_host panel
 *
 * @param[in]  panel Panel handle created by esp_lcd_new_panel_host_sdl()
 * @param[out] info  Returned panel state
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for an unknown handle
 */
esp_err_t esp_lcd_host_panel_get_info(esp_lcd_panel_handle_t panel, esp_lcd_host_panel_info_t *info);

#ifdef __cplusplus
} // extern "C"
#endif
