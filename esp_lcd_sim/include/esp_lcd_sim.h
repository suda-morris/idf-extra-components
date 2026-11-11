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
 * @brief SDL sim panel configuration structure
 *
 * @note `width` and `height` describe the framebuffer the GUI renders into.
 *       The preview window shows that frame at `width` x `height` logical
 *       pixels, scaled by `scale`. With `show_board_frame` the window instead
 *       shows a development board with a screen cut-out of exactly
 *       `width` x `height`, so the GUI appears inside that screen at 1:1.
 *       The exported PNG always uses just the `width` x `height` framebuffer
 *       and never includes the board frame.
 */
typedef struct {
    int width;                       /*!< Panel width in pixels */
    int height;                      /*!< Panel height in pixels */
    int scale;                       /*!< Integer upscale factor for the preview, 0 or 1 means no scaling */
    esp_color_fourcc_t color_format; /*!< Framebuffer color format: `ESP_COLOR_FOURCC_RGB16`, `ESP_COLOR_FOURCC_RGB16_BE`,
                                          `ESP_COLOR_FOURCC_RGB24`, `ESP_COLOR_FOURCC_BGR24` or `ESP_COLOR_FOURCC_BGRA32` */
    bool create_window;              /*!< Open a resizable SDL preview window. Without a display the panel falls back
                                          to framebuffer-only with a warning; set false to never try opening one. */
    bool show_board_frame;           /*!< Draw a development-board frame around the LCD in the preview window.
                                          The board look is fixed; the screen keeps the exact panel resolution, so the
                                          GUI is shown inside it. Only affects the preview;
                                          esp_lcd_sim_screenshot_save_png() still saves just the LCD.
                                          Ignored when `create_window` is false. If the board texture cannot be built,
                                          the preview falls back to a plain LCD view. */
    const char *window_title;        /*!< Preview window title, NULL selects a default */
    int screenshot_key;              /*!< Save a PNG when this key is pressed in the preview window, 0 disables it.
                                          An ASCII letter matches with or without Shift, anything else must match
                                          exactly. An SDL keycode; the ASCII letters have their ASCII value.
                                          A held-down key does not retrigger the screenshot. */
    const char *screenshot_path;     /*!< Base file name the screenshot key writes to, NULL selects "screenshot.png".
                                          Presses never overwrite an existing file: the first free name is used,
                                          starting with this one, then "shot_001.png", "shot_002.png", ... before
                                          the extension. esp_lcd_sim_screenshot_save_png() instead always writes
                                          the exact path it is given. The string is copied by the panel and need
                                          not outlive the call. */
} esp_lcd_sim_config_t;

/**
 * @brief Create an esp_lcd panel backed by SDL
 *
 * The panel implements the `esp_lcd_panel_t` interface, so a GUI port written
 * for a real LCD can be pointed at this device without further changes. Every
 * `draw_bitmap()` call is stored in a full size framebuffer, which
 *   - can be exported with `esp_lcd_sim_screenshot_save_png()`,
 *   - is shown in an SDL preview window when `config->create_window` is set.
 *
 * The configuration is stored per panel and every panel keeps its own
 * framebuffer, so panels with different resolutions can coexist.
 *
 * When `config->create_window` is set but no preview window can be created
 * (for example on a machine without a display), the panel is still created
 * and works framebuffer-only; a warning is logged and
 * esp_lcd_sim_window_is_open() reports false.
 *
 * @param[in]  config     Panel configuration
 * @param[out] ret_panel  Returned panel handle
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if config or ret_panel is NULL, the dimensions or scale overflow, or the color format is not supported
 *      - ESP_ERR_NO_MEM if memory allocation for the panel, the framebuffer or the preview fails
 */
esp_err_t esp_lcd_new_panel_sim_sdl(const esp_lcd_sim_config_t *config, esp_lcd_panel_handle_t *ret_panel);

/**
 * @brief Pump the SDL event queue of the preview windows
 *
 * Each call does two things for every preview window of the process:
 *
 * - pumps the shared SDL event queue (`SDL_PumpEvents()`) and drains it
 *   (`SDL_PollEvent()`), acting on the quit, key-down and window-close-request
 *   events. A close request destroys the preview window: SDL reports a close
 *   request instead of destroying the window, so an unread one would leave a
 *   dead window on screen. Check esp_lcd_sim_window_close_requested()
 *   afterwards to stop rendering when the user is done.
 * - presents the windows that were drawn to since the previous call: the
 *   drawn regions are uploaded into the preview texture
 *   (`SDL_UpdateTexture()`) and the frame is presented
 *   (`SDL_RenderPresent()`).
 *
 * Applications must call it periodically (for example once per LVGL timer
 * handler) while a preview window is open. Without a preview window there is
 * nothing to drain and nothing to present.
 *
 * The whole queue is drained, not only the three events acted on, so that
 * esp_lcd_sim_wait_events() can block until new input arrives. Pointer input
 * does not come from the queue: esp_lcd_sim_mouse_get() samples SDL's cached
 * mouse state directly, so discarding the mouse events loses nothing. The
 * queue is process-wide, so an application that also uses SDL should
 * coordinate its own event loop with this one.
 *
 * @note Like every function of this component, this must be called from the
 *       same thread that created the panels: SDL windows and its event queue
 *       are not thread-safe.
 *
 * @return
 *      - ESP_OK: always; a failure inside SDL is logged, not reported here
 */
esp_err_t esp_lcd_sim_pump_events(void);

/**
 * @brief Block until a preview event arrives or a timeout expires
 *
 * The idle half of an event-driven preview loop: instead of sleeping a fixed
 * frame period, the application waits here and is woken by input (a key, a
 * mouse move, a window close) or by the timeout, whichever comes first. The
 * events are left in the queue for the next esp_lcd_sim_pump_events() call, so
 * a typical loop alternates the two:
 * @code
 * while (!esp_lcd_sim_window_close_requested()) {
 *     esp_lcd_sim_pump_events();
 *     uint32_t idle_ms = lv_timer_handler();   // ms until the next LVGL timer
 *     esp_lcd_sim_wait_events(idle_ms);
 * }
 * @endcode
 * An idle GUI then costs almost no CPU and still reacts to input promptly.
 *
 * With no preview window (framebuffer-only panel) there is nothing to wait
 * for, so the call returns immediately.
 *
 * @note Like every function of this component, this must be called from the
 *       same thread that created the panels.
 *
 * @param[in] timeout_ms Maximum time to wait in milliseconds. 0 polls without
 *                       blocking; values above INT32_MAX are clamped to it.
 * @return
 *      - ESP_OK: always
 */
esp_err_t esp_lcd_sim_wait_events(uint32_t timeout_ms);

/**
 * @brief Release the global SDL resources of the component
 *
 * The component initializes the SDL video subsystem when the first preview
 * window is created and keeps it until this function is called. Delete every
 * panel created by esp_lcd_new_panel_sim_sdl() first, then call this once
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
esp_err_t esp_lcd_sim_deinit(void);

/**
 * @brief Whether the user closed a preview window
 *
 * The flag is set when the user closes any preview window of the process (or
 * quits the application through the window manager) and then stays set, so a
 * periodic check cannot miss it. A typical host application loop stops
 * rendering and tears the panel down once this returns true:
 * @code
 * while (!esp_lcd_sim_window_close_requested()) {
 *     esp_lcd_sim_pump_events();
 *     esp_lcd_sim_wait_events(lv_timer_handler());
 * }
 * @endcode
 * The panel framebuffer keeps working after the window is gone, so a final
 * esp_lcd_sim_screenshot_save_png() still captures the last frame.
 *
 * @return true if a preview window was closed by the user, false otherwise
 */
bool esp_lcd_sim_window_close_requested(void);

/**
 * @brief Whether a panel currently has an open preview window
 *
 * Returns false when the panel was created with `create_window` unset, when
 * no window could be created (the panel then fell back to framebuffer-only)
 * and after the user closed the window. A typical application checks it once
 * after esp_lcd_new_panel_sim_sdl() to choose between an interactive preview
 * loop and a headless render-and-export run.
 *
 * @note Like every function of this component, this must be called from the
 *       same thread that created the panels.
 *
 * @param[in] panel Panel handle created by esp_lcd_new_panel_sim_sdl()
 * @return true if the panel has an open preview window, false otherwise
 *         (including a NULL or foreign panel handle)
 */
bool esp_lcd_sim_window_is_open(esp_lcd_panel_handle_t panel);

/**
 * @brief Pointer (mouse) state of a panel's preview window
 *
 * The host mouse plays the finger on the simulated touch screen: the pointer
 * position is mapped from the preview window into panel pixels, and the left
 * button plays the touch contact.
 */
typedef struct {
    bool pressed;   /*!< True while the left button is held and the pointer is on the screen area of the preview.
                         With `show_board_frame` the board around the LCD counts as the bezel, not the glass. */
    int x;          /*!< Pointer column in panel pixels: the current position while the pointer is over the screen
                         area, otherwise the last position it had there (0,0 before the first visit), so a GUI
                         library also gets hover information. */
    int y;          /*!< Pointer row in panel pixels, see `x`. */
} esp_lcd_sim_mouse_data_t;

/**
 * @brief Read the pointer state of a panel's preview window
 *
 * While the pointer is over the screen area of the preview window (the LCD
 * cut-out of the board with `show_board_frame`, the whole window otherwise),
 * `data->x` and `data->y` hold the position in panel pixels and
 * `data->pressed` reports the left button. A pointer on the board or on
 * another window releases the touch and keeps the last on-screen position.
 *
 * The state is SDL's knowledge as of the last esp_lcd_sim_pump_events() call,
 * so poll it in the same loop that pumps (an LVGL input device read callback
 * is a natural fit, see the example). A panel without a preview window
 * reports released at the last position sampled on the screen, (0,0) if the
 * pointer never was there.
 *
 * @note Like every function of this component, this must be called from the
 *       same thread that created the panels.
 *
 * @param[in]  panel Panel handle created by esp_lcd_new_panel_sim_sdl()
 * @param[out] data  Returned pointer state
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if data is NULL, or panel is NULL or is not a SDL sim panel
 */
esp_err_t esp_lcd_sim_mouse_get(esp_lcd_panel_handle_t panel, esp_lcd_sim_mouse_data_t *data);

/**
 * @brief Event callbacks of the simulated panel
 *
 * The panel fires `on_flush_done` where a real transfer-based display driver
 * reports the completion of a color transfer — the analog of
 * `esp_lcd_panel_io_event_callbacks_t::on_color_trans_done` of an SPI LCD:
 * synchronously after esp_lcd_panel_draw_bitmap() has copied the pixels into
 * the panel framebuffer. A partial refresh flushes several areas, so it
 * fires once per draw call, not once per frame. A GUI port calls
 * lv_display_flush_ready() from it, exactly like it does from the transfer
 * done callback on hardware.
 *
 * The callback runs synchronously on the thread that called the triggering
 * function (the usual single-thread contract of this component) and must not
 * call back into the drawing functions of the same panel.
 */
typedef struct {
    void (*on_flush_done)(esp_lcd_panel_handle_t panel, void *user_ctx); /*!< After each draw_bitmap reached the framebuffer */
} esp_lcd_sim_event_callbacks_t;

/**
 * @brief Register event callbacks of a simulated panel
 *
 * The callbacks are copied into the panel and the pointer is not kept, so the
 * structure can be a temporary. Pass NULL as `callbacks` to unregister.
 * Safe to call at any time after esp_lcd_new_panel_sim_sdl(), including
 * after the preview window has been closed.
 *
 * @param[in] panel     Panel handle created by esp_lcd_new_panel_sim_sdl()
 * @param[in] callbacks Callback structure, NULL unregisters
 * @param[in] user_ctx  Passed to every callback as `user_ctx`
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if panel is NULL or is not a SDL sim panel
 */
esp_err_t esp_lcd_sim_register_event_callbacks(esp_lcd_panel_handle_t panel,
                                               const esp_lcd_sim_event_callbacks_t *callbacks, void *user_ctx);

/**
 * @brief Save the current panel content to a file in PNG format
 *
 * Colors are converted scanline by scanline and fed to libpng (`png_write_row`),
 * so no extra full frame RGB buffer is allocated. The output uses the `width`,
 * `height` and `color_format` given at creation time and is therefore
 * independent of the preview scaling. Call it after the GUI has finished the
 * frame you want, drawing is not paused.
 *
 * @param[in] panel    Panel handle created by esp_lcd_new_panel_sim_sdl()
 * @param[in] filepath Path of the PNG file to write
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if filepath is NULL, or panel is NULL or is not a SDL sim panel
 *      - ESP_ERR_NO_MEM if memory allocation for the PNG encoder or scanline buffer fails
 *      - ESP_FAIL if filepath cannot be opened or the PNG file cannot be written
 */
esp_err_t esp_lcd_sim_screenshot_save_png(esp_lcd_panel_handle_t panel, const char *filepath);

#ifdef __cplusplus
}
#endif
