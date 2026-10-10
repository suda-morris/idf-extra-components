/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SDL host panel configuration structure
 *
 * @note `width` and `height` describe the framebuffer the GUI renders into.
 *       When `scale > 1` the preview window and the optional SDL renderer
 *       output are `width * scale` x `height * scale`.
 */
typedef struct {
    int width;                       /*!< Panel width in pixels */
    int height;                      /*!< Panel height in pixels */
    int scale;                       /*!< Integer upscale factor for the preview, 0 or 1 means no scaling */
    esp_color_fourcc_t color_format; /*!< Framebuffer color format: `ESP_COLOR_FOURCC_RGB16`, `ESP_COLOR_FOURCC_RGB16_BE`,
                                          `ESP_COLOR_FOURCC_RGB24`, `ESP_COLOR_FOURCC_BGR24` or `ESP_COLOR_FOURCC_BGRA32` */
    bool create_window;              /*!< Open a resizable SDL preview window (needs a display, keep false in CI) */
    const char *window_title;        /*!< Preview window title, NULL selects a default */
    int screenshot_key;              /*!< Save a PNG when this key is pressed in the preview window, 0 disables it.
                                          An ASCII letter matches with or without Shift, anything else must match
                                          exactly. An SDL keycode; the ASCII letters have their ASCII value. */
    const char *screenshot_path;     /*!< File the key writes to, NULL selects "screenshot.png".
                                          The string is copied by the panel and need not outlive the call. */
} esp_lcd_host_config_t;

/**
 * @brief Create an esp_lcd panel backed by SDL
 *
 * The panel implements the `esp_lcd_panel_t` interface, so a GUI port written
 * for a real LCD can be pointed at this device without further changes. Every
 * `draw_bitmap()` call is stored in a full size framebuffer, which
 *   - can be exported with `esp_lcd_host_screenshot_save_png()`,
 *   - is shown in an SDL preview window when `config->create_window` is set.
 *
 * The configuration is stored per panel and every panel keeps its own
 * framebuffer, so panels with different resolutions can coexist.
 *
 * @param[in]  config     Panel configuration
 * @param[out] ret_panel  Returned panel handle
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if config or ret_panel is NULL, the dimensions or scale overflow, or the color format is not supported
 *      - ESP_ERR_NO_MEM if memory allocation for the panel, the framebuffer or the preview fails
 *      - ESP_FAIL if SDL could not create the preview window or its rendering resources (for example on a machine without a display); the panel still works without the window
 */
esp_err_t esp_lcd_new_panel_host_sdl(const esp_lcd_host_config_t *config, esp_lcd_panel_handle_t *ret_panel);

/**
 * @brief Pump the SDL event queue of the preview windows
 *
 * Each call does two things for every preview window of the process:
 *
 * - pumps the shared SDL event queue (`SDL_PumpEvents()`) and takes the
 *   quit, key-down and window-close-request events from it
 *   (`SDL_PeepEvents()`). A close request destroys the preview window: SDL
 *   reports a close request instead of destroying the window, so an unread
 *   one would leave a dead window on screen. Check
 *   esp_lcd_host_window_close_requested() afterwards to stop rendering when
 *   the user is done.
 * - presents the windows that were drawn to since the previous call: the
 *   union of the drawn regions is uploaded into the preview texture
 *   (`SDL_UpdateTexture()`) and the frame is presented
 *   (`SDL_RenderPresent()`).
 *
 * Applications must call it periodically (for example once per LVGL timer
 * handler) while a preview window is open. Without a preview window there is
 * nothing to drain and nothing to present.
 *
 * Only those three event types are removed from the queue; events of other
 * types are left in place for other SDL consumers. The simulator has no
 * general input API yet, so the configured screenshot key is the only input
 * acted on. The queue is nevertheless process-wide, so an application that
 * also uses SDL should coordinate its own event loop with this one.
 *
 * @note Like every function of this component, this must be called from the
 *       same thread that created the panels: SDL windows and its event queue
 *       are not thread-safe.
 *
 * @return
 *      - ESP_OK: always; a failure inside SDL is logged, not reported here
 */
esp_err_t esp_lcd_host_pump_events(void);

/**
 * @brief Release the global SDL resources of the component
 *
 * The component initializes the SDL video subsystem when the first preview
 * window is created and keeps it until this function is called. Delete every
 * panel created by esp_lcd_new_panel_host_sdl() first, then call this once
 * before the process exits (or before using SDL for something else). It also
 * clears the close-requested flag, so a later panel starts from a clean state.
 *
 * This quits SDL as a whole, not just the video subsystem: SDL subsystem
 * refcounts are owned by SDL, and SDL_Quit() is the only teardown that
 * reliably releases them. Do not call it while another part of the process
 * still uses SDL.
 *
 * @note Like every function of this component, this must be called from the
 *       same thread that created the panels.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if a panel with a live preview backend still exists
 */
esp_err_t esp_lcd_host_deinit(void);

/**
 * @brief Whether the user closed a preview window
 *
 * The flag is set when the user closes any preview window of the process (or
 * quits the application through the window manager) and then stays set, so a
 * periodic check cannot miss it. A typical host application loop stops
 * rendering and tears the panel down once this returns true:
 * @code
 * while (!esp_lcd_host_window_close_requested()) {
 *     esp_lcd_host_pump_events();
 *     lv_timer_handler();
 * }
 * @endcode
 * The panel framebuffer keeps working after the window is gone, so a final
 * esp_lcd_host_screenshot_save_png() still captures the last frame.
 *
 * @return true if a preview window was closed by the user, false otherwise
 */
bool esp_lcd_host_window_close_requested(void);

/**
 * @brief Save the current panel content to a file in PNG format
 *
 * Colors are converted scanline by scanline and fed to libpng (`png_write_row`),
 * so no extra full frame RGB buffer is allocated. The output uses the `width`,
 * `height` and `color_format` given at creation time and is therefore
 * independent of the preview scaling. Call it after the GUI has finished the
 * frame you want, drawing is not paused.
 *
 * @param[in] panel    Panel handle created by esp_lcd_new_panel_host_sdl()
 * @param[in] filepath Path of the PNG file to write
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if filepath is NULL, or panel is NULL or is not a host SDL panel
 *      - ESP_ERR_NO_MEM if memory allocation for the PNG encoder or scanline buffer fails
 *      - ESP_FAIL if filepath cannot be opened or the PNG file cannot be written
 */
esp_err_t esp_lcd_host_screenshot_save_png(esp_lcd_panel_handle_t panel, const char *filepath);

#ifdef __cplusplus
}
#endif
