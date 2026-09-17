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
    ESP_LCD_HOST_TARGET_ESP32 = 0,  /*!< Real Espressif chip; the SDL simulator runs as a separate process */
    ESP_LCD_HOST_TARGET_POSIX,      /*!< ESP-IDF host/POSIX (Linux) target, app and simulator share one process */
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
    esp_color_fourcc_t color_format;    /*!< Framebuffer color format, see esp_lcd_screenshot.h for the supported formats */
    bool create_window;                 /*!< Open a resizable SDL preview window (needs a display, keep false in CI) */
    bool use_renderer;                  /*!< Let SDL scale the preview. Defaults to true when `create_window` is set */
    int scale;                          /*!< Integer upscale factor for the preview, 0 or 1 means no scaling */
    const char *window_title;           /*!< Preview window title, NULL selects a default */
} esp_lcd_host_config_t;

/**
 * @brief Create an esp_lcd panel backed by SDL
 *
 * The panel implements the `esp_lcd_panel_t` interface expected by the `esp_lcd`
 * component, so a GUI port written for a real LCD can be pointed at this device
 * without further changes. Every `draw_bitmap()` call is stored in a full size
 * framebuffer, which
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
 * @brief Return the panel to the SDL simulation backend
 *
 * On the POSIX target the SDL simulation backend shares the host process with
 * the application, so returning the latest frame is what makes an external
 * simulator show it:
 * @code
 * esp_lcd_panel_handle_t panel = NULL;
 * esp_lcd_new_panel_host_sdl(&config, &panel);
 * ESP_ERROR_CHECK(esp_lcd_host_return_panel(panel));
 * ESP_ERROR_CHECK(esp_lcd_host_return_buffers(panel));
 * @endcode
 * On a real chip the calls are accepted and do nothing, because the SDL panel is
 * a local stand-in for a panel owned by the simulator process.
 *
 * @param[in] panel Panel handle created by esp_lcd_new_panel_host_sdl()
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if panel is NULL, or is not a host SDL panel
 */
esp_err_t esp_lcd_host_return_panel(esp_lcd_panel_handle_t panel);

/**
 * @brief Hand the panel framebuffer back to the SDL simulation backend
 *
 * See esp_lcd_host_return_panel(). Call it after the GUI has finished the frame
 * you want the simulator to show.
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
 * call is a no-op.
 *
 * @return
 *      - ESP_OK: all preview windows were updated
 *      - ESP_FAIL: an SDL error occurred, use SDL_GetError() for details
 */
esp_err_t esp_lcd_host_pump_events(void);

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
