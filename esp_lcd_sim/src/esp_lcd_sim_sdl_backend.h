/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "esp_lcd_sim.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The SDL preview backend of the panel.
 *
 * One implementation, not a plugin interface: the split is about ownership.
 * The panel object (esp_lcd_sim.c) owns the framebuffer, the touch override
 * and the user callbacks; the backend owns every SDL object and the event
 * queue, and the panel issues every backend call.
 *
 * All backend functions must be called from the GUI thread: SDL windows and
 * its event queue are not thread-safe, so the backend adds no locking of its
 * own. The panel object does keep a per-panel mutex, but it only guards the
 * framebuffer pixels and the touch override so that a command thread can
 * inject touch and export a frame while the GUI thread draws. It never guards
 * the backend state below, which stays on the GUI thread.
 *
 * A backend context has three states, and every entry point handles all of
 * them:
 *   - NULL: no preview at all, because create_window was false or the preview
 *     could not be created (no display, SDL failure). Every call is a no-op;
 *     the panel works framebuffer-only.
 *   - non-NULL with a window: the live preview.
 *   - non-NULL without a window: the user closed the preview. The backend
 *     stays registered as long as its panel lives, but holds no SDL objects:
 *     it presents nothing and tracks no dirty regions, while the panel keeps
 *     rendering into its framebuffer and exporting PNGs.
 */

/**
 * @brief Callback into the panel, invoked by the backend
 *
 * The backend owns the SDL objects and events, while the panel owns the
 * framebuffer and the user callbacks, so the panel hands these functions over
 * and the backend calls them from its own paths (screenshot key press, frame
 * presentation).
 *
 * @param[in] user_ctx  Context handed to the backend together with the function
 */
typedef void (*esp_lcd_sim_backend_cb_t)(void *user_ctx);

/**
 * @brief Create the SDL objects backing one panel
 *
 * The backend uploads the framebuffer of the panel into a texture, so the
 * panel never includes an SDL header; the SDL pixel format it hands down is
 * resolved by esp_lcd_sim_sdl_pixel_format() below.
 *
 * @param[in]  config        Panel configuration, read by the backend
 * @param[in]  framebuffer   Panel framebuffer, source of the preview uploads
 * @param[in]  framebuffer_size Size of the framebuffer in bytes
 * @param[in]  sdl_format    SDL pixel format matching the framebuffer layout,
 *                           as returned by esp_lcd_sim_sdl_pixel_format()
 * @param[in]  bytes_per_pixel Bytes per pixel of the framebuffer format
 * @param[in]  screenshot_cb Called when the screenshot key is pressed in the
 *                           preview window, with `screenshot_ctx`; ignored
 *                           when the panel has no preview window
 * @param[in]  screenshot_ctx Context passed to `screenshot_cb`
 * @param[out] ret_ctx       Returned backend context, NULL when the panel ends up without a
 *                           preview (see the state model above)
 * @return
 *      - ESP_OK on success. When the preview cannot be created (no display,
 *        SDL window/renderer/texture failure) the backend logs a warning and
 *        returns ESP_OK with *ret_ctx NULL: the panel works framebuffer-only.
 *      - ESP_ERR_NO_MEM if a SDL resource or the internal buffer cannot be allocated
 */
esp_err_t esp_lcd_sim_sdl_backend_create(const esp_lcd_sim_config_t *config, uint8_t *framebuffer,
                                          size_t framebuffer_size, uint32_t sdl_format, size_t bytes_per_pixel,
                                          esp_lcd_sim_backend_cb_t screenshot_cb, void *screenshot_ctx,
                                          void **ret_ctx);

/**
 * @brief Release one SDL backend, called from esp_lcd_panel_del()
 */
void esp_lcd_sim_sdl_backend_delete(void *ctx);

/**
 * @brief Push the latest panel content to the preview
 *
 * Marks the whole panel dirty: the next esp_lcd_sim_pump_events() uploads the
 * complete framebuffer into the preview texture and presents it.
 */
esp_err_t esp_lcd_sim_sdl_backend_update(void *ctx, const uint8_t *framebuffer, size_t framebuffer_size);

/**
 * @brief Push a panel region to the preview
 *
 * Marks the region dirty, merging it with the pending regions it touches, so
 * a GUI that flushes in partial areas uploads only what changed on the next
 * pump call. A no-op without a live preview texture: nothing would ever
 * upload the region, so tracking it would only cost CPU on every flush.
 *
 * @param[in] x      Left edge of the region in panel pixels
 * @param[in] y      Top edge of the region in panel pixels
 * @param[in] width  Width of the region in pixels
 * @param[in] height Height of the region in pixels
 */
esp_err_t esp_lcd_sim_sdl_backend_update_rect(void *ctx, const uint8_t *framebuffer, size_t framebuffer_size,
                                               int x, int y, int width, int height);

/**
 * @brief Pump the SDL event queue, called from esp_lcd_sim_pump_events()
 */
esp_err_t esp_lcd_sim_sdl_backend_pump(void);

/**
 * @brief Block until an SDL event or a timeout, called from esp_lcd_sim_wait_events()
 *
 * Leaves any event in the queue for the next pump. Without an SDL event
 * source (framebuffer-only build) it sleeps for the timeout instead, so the
 * caller idles the same way with and without a preview.
 */
esp_err_t esp_lcd_sim_sdl_backend_wait_events(uint32_t timeout_ms);

/**
 * @brief Sample the pointer state of the backend's preview window
 *
 * Called by esp_lcd_sim_input_get(). The host mouse position is mapped into
 * panel pixels through the renderer's logical presentation; a position on
 * the board but off the screen cut-out reports released. `backend` NULL (a
 * panel without a preview window) always reports released.
 */
esp_err_t esp_lcd_sim_sdl_backend_input_get(void *backend, esp_lcd_sim_input_data_t *data);

/**
 * @brief Whether the backend's preview window is open
 *
 * Called by esp_lcd_sim_window_is_open(). False for a NULL backend (a panel
 * that never got a preview) and after the window was closed. It answers "is
 * there a window", not "does the panel work": a framebuffer-only panel keeps
 * rendering and exporting with this returning false.
 */
bool esp_lcd_sim_sdl_backend_window_is_open(void *backend);

/**
 * @brief Release the global SDL state, called from esp_lcd_sim_deinit()
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if a live backend remains
 */
esp_err_t esp_lcd_sim_sdl_backend_deinit(void);

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
esp_err_t esp_lcd_sim_sdl_pixel_format(esp_color_fourcc_t fourcc, uint32_t *ret_format, size_t *ret_bytes);

#ifdef __cplusplus
} // extern "C"
#endif
