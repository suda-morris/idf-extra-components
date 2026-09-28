/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_host_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Target the panel simulator is running on
 */
typedef enum {
    ESP_LCD_HOST_TARGET_ESP32 = 0,  /*!< Real Espressif chip; the component does not build for this target */
    ESP_LCD_HOST_TARGET_POSIX,      /*!< ESP-IDF host/POSIX (Linux) target, the SDL backend renders in the app */
} esp_lcd_host_target_t;

/**
 * @brief SDL host panel configuration structure
 *
 * @note `width` and `height` describe the framebuffer the GUI renders into.
 *       When `scale > 1` the preview window and the optional SDL renderer
 *       output are `width * scale` x `height * scale`.
 */
typedef struct {
    int width;                          /*!< Panel width in pixels */
    int height;                         /*!< Panel height in pixels */
    esp_color_fourcc_t color_format;    /*!< Framebuffer color format, one of the `esp_color_fourcc_t` values of the HAL */
    bool create_window;                 /*!< Open a resizable SDL preview window (needs a display, keep false in CI) */
    int scale;                          /*!< Integer upscale factor for the preview, 0 or 1 means no scaling */
    const char *window_title;           /*!< Preview window title, NULL selects a default */
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
 *      - ESP_ERR_INVALID_ARG if config or ret_panel is NULL, dimensions overflow, the color format is not supported, or an SDL resource cannot be created with the given parameters
 *      - ESP_ERR_NO_MEM if memory allocation for the panel or the framebuffer fails
 */
esp_err_t esp_lcd_new_panel_host_sdl(const esp_lcd_host_config_t *config, esp_lcd_panel_handle_t *ret_panel);

/**
 * @brief Get the target the panel simulator is running on
 *
 * Useful for code that wants to adapt to a simulated panel, for example to slow
 * down animation ticks so the rendered frames stay readable.
 *
 * @param[in]  panel  Panel handle created by esp_lcd_new_panel_host_sdl()
 * @param[out] target Returned target descriptor
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if panel or target is NULL, or panel is not a host SDL panel
 */
esp_err_t esp_lcd_host_get_target(esp_lcd_panel_handle_t panel, esp_lcd_host_target_t *target);

/**
 * @brief Publish the latest frame of the panel
 *
 * The SDL backend renders the framebuffer of the panel, so the frame is already
 * visible in the preview window and the call only checks the handle:
 * @code
 * esp_lcd_panel_handle_t panel = NULL;
 * esp_lcd_new_panel_host_sdl(&config, &panel);
 * ESP_ERROR_CHECK(esp_lcd_host_return_panel(panel));
 * ESP_ERROR_CHECK(esp_lcd_host_return_buffers(panel));
 * @endcode
 * It is kept so a GUI port that hands its frame to a display driver can use one
 * code path for the host build and can keep the call after switching to a real
 * panel.
 *
 * @param[in] panel Panel handle created by esp_lcd_new_panel_host_sdl()
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if panel is NULL, or is not a host SDL panel
 */
esp_err_t esp_lcd_host_return_panel(esp_lcd_panel_handle_t panel);

/**
 * @brief Publish the panel framebuffer
 *
 * See esp_lcd_host_return_panel(). Call it after the GUI has finished the frame
 * you want to publish.
 *
 * @param[in] panel Panel handle created by esp_lcd_new_panel_host_sdl()
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if panel is NULL, or is not a host SDL panel
 */
esp_err_t esp_lcd_host_return_buffers(esp_lcd_panel_handle_t panel);

/**
 * @brief Pump the SDL event queue of the preview windows
 *
 * Uses `SDL_UpdateWindowSurface()` and `SDL_PumpEvents()` to keep the preview
 * windows responsive. Applications must call it periodically (for example once
 * per LVGL timer handler) while a preview window is open. Without a window the
 * call only pumps the event queue.
 *
 * The call also drains the SDL event queue and destroys a preview window when
 * the user closes it, otherwise the close request would go unnoticed and a
 * dead window would stay on screen. Check esp_lcd_host_window_close_requested()
 * afterwards to stop rendering when the user is done.
 *
 * @return
 *      - ESP_OK: all preview windows were updated
 *      - ESP_FAIL: an SDL error occurred, use SDL_GetError() for details
 */
esp_err_t esp_lcd_host_pump_events(void);

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
