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

/*
 * Thread safety: the API is split into a GUI-thread-only group (everything
 * that touches SDL or the panel lifecycle) and an any-thread group (touch
 * injection and PNG export, guarded by a per-panel mutex). Each function
 * below notes its group. The model, the guarantees and the lifecycle rules
 * are documented in the programming guide, docs/src/threading.md.
 */

/**
 * @brief SDL sim panel configuration structure
 *
 * `width` x `height` is the framebuffer the GUI renders into. The preview
 * window shows it scaled by `scale`, optionally inside a development-board
 * frame (`show_board_frame`); the exported PNG always uses just the
 * framebuffer, never the board.
 */
typedef struct {
    int width;                       /*!< Panel width in pixels */
    int height;                      /*!< Panel height in pixels */
    int scale;                       /*!< Integer upscale factor for the preview, 0 or 1 means no scaling */
    esp_color_fourcc_t color_format; /*!< Framebuffer color format: `ESP_COLOR_FOURCC_RGB16`, `ESP_COLOR_FOURCC_RGB16_BE`,
                                          `ESP_COLOR_FOURCC_RGB24`, `ESP_COLOR_FOURCC_BGR24` or `ESP_COLOR_FOURCC_BGRA32` */
    bool create_window;              /*!< Open an SDL preview window; without a display the panel falls back to
                                          framebuffer-only with a warning. Set false to never try opening one. */
    bool show_board_frame;           /*!< Show the LCD inside a fixed development-board frame in the preview. The screen
                                          keeps the exact panel resolution. Preview only: the PNG export is unaffected.
                                          Ignored when `create_window` is false; falls back to a plain LCD view if the
                                          board texture cannot be built. */
    const char *window_title;        /*!< Preview window title, NULL selects a default */
    int screenshot_key;              /*!< Save a PNG when this key is pressed in the preview, 0 disables it. An SDL
                                          keycode; ASCII letters match with or without Shift. A held key does not retrigger. */
    const char *screenshot_path;     /*!< Base file name for screenshot-key exports, NULL selects "screenshot.png".
                                          Presses never overwrite: the first free name is used, then "name_001.png",
                                          "name_002.png", ... Copied by the panel. esp_lcd_sim_screenshot_save_png()
                                          instead always writes the exact path it is given. */
} esp_lcd_sim_config_t;

/**
 * @brief Create an esp_lcd panel backed by SDL
 *
 * The panel implements the `esp_lcd_panel_t` interface, so a GUI port written
 * for a real LCD works without changes: every `draw_bitmap()` call is stored
 * in a full-size framebuffer, which can be exported as a PNG and is shown in
 * an SDL preview window when `config->create_window` is set. Panels with
 * different resolutions can coexist.
 *
 * @note When no preview window can be created (for example on a machine
 *       without a display), the panel still works framebuffer-only; a warning
 *       is logged and esp_lcd_sim_window_is_open() reports false.
 * @note There is no hardware to program: reset, mirror, swap_xy, set_gap,
 *       invert_color, disp_on_off, disp_sleep and set_brightness are accepted
 *       as no-ops (the ones that would change the picture log a warning), so
 *       a GUI port that wraps every panel operation in ESP_ERROR_CHECK() runs
 *       unchanged.
 * @note GUI thread only.
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
 * @brief Pump the SDL event queue and present the preview windows
 *
 * Drains the process-wide SDL event queue, acting on quit, key-down and
 * window-close events, and presents every window drawn to since the previous
 * call. A close request destroys that preview window; the panel's
 * esp_lcd_sim_window_is_open() then reports false, which stops a GUI loop
 * that runs while the window is open.
 * Call periodically from the GUI loop while a preview window is open.
 *
 * @note GUI thread only.
 *
 * @return
 *      - ESP_OK: always; a failure inside SDL is logged, not reported here
 */
esp_err_t esp_lcd_sim_pump_events(void);

/**
 * @brief Block until a preview event arrives or a timeout expires
 *
 * The idle half of an event-driven GUI loop: the events are left in the queue
 * for the next esp_lcd_sim_pump_events() call, so a typical loop alternates
 * the two (see the programming guide). Without a preview window there is no
 * event queue to wait on, so the call sleeps for the timeout instead: one
 * loop idles correctly with and without a window.
 *
 * @note GUI thread only.
 * @note A touch injected from another thread does not wake the sleep, which
 *       is rounded up to whole FreeRTOS ticks. A headless loop that has to
 *       stay responsive to injection therefore passes a bounded timeout
 *       instead of the full idle time its GUI library reports.
 *
 * @param[in] timeout_ms Maximum time to wait in milliseconds, 0 returns
 *                       without blocking.
 * @return
 *      - ESP_OK: always
 */
esp_err_t esp_lcd_sim_wait_events(uint32_t timeout_ms);

/**
 * @brief Release the global SDL resources of the component
 *
 * Delete every sim panel first, then call this once before the process exits
 * (or before using SDL for something else). It quits SDL as a whole.
 *
 * @note GUI thread only. Do not call while another part of the process still
 *       uses SDL.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if a panel with a live preview backend still exists
 */
esp_err_t esp_lcd_sim_deinit(void);

/**
 * @brief Whether a panel currently has an open preview window
 *
 * False when `create_window` was unset, when no window could be created
 * (framebuffer-only fallback), and after the user closed the window or quit
 * the application through the window manager. Check it once after creation
 * to choose between an interactive preview loop and a headless
 * render-and-export run; an interactive loop runs while it returns true.
 * A headless run has no window to close, so it stops on its own condition,
 * for example a console quit flag; esp_lcd_sim_wait_events() idles it just
 * the same. This reports the window, not the panel: a framebuffer-only panel
 * keeps rendering and exporting PNGs while this returns false.
 *
 * @note GUI thread only.
 *
 * @param[in] panel Panel handle created by esp_lcd_new_panel_sim_sdl()
 * @return true if the panel has an open preview window, false otherwise
 *         (including a NULL or foreign panel handle)
 */
bool esp_lcd_sim_window_is_open(esp_lcd_panel_handle_t panel);

/**
 * @brief Pointer (mouse) state of a panel, in panel pixels
 *
 * The host mouse plays the finger on the simulated touch screen; an injected
 * touch (esp_lcd_sim_input_inject_touch()) is reported through the same
 * structure.
 */
typedef struct {
    bool pressed;   /*!< True while the contact is held and the pointer is on the screen area of the preview.
                         With `show_board_frame` the board around the LCD counts as the bezel, not the glass. */
    int x;          /*!< Pointer column in panel pixels; off the screen area the last on-screen position is kept
                         (0,0 before the first visit), so a GUI library also gets hover information. */
    int y;          /*!< Pointer row in panel pixels, see `x`. */
} esp_lcd_sim_input_data_t;

/**
 * @brief Read the pointer state of a panel
 *
 * Reports the preview window's mouse mapped into panel pixels, the left
 * button as the touch contact, as of the last esp_lcd_sim_pump_events() call.
 * While a touch is injected, the injected state is reported instead, until
 * esp_lcd_sim_input_inject_reset(). A panel without a preview window reports
 * released at the last on-screen position, (0,0) if the pointer never was there.
 *
 * @note GUI thread only (it samples SDL). The injected override it may report
 *       is synchronized with the thread that injects it.
 *
 * @param[in]  panel Panel handle created by esp_lcd_new_panel_sim_sdl()
 * @param[out] data  Returned pointer state
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if data is NULL, or panel is NULL or is not a SDL sim panel
 */
esp_err_t esp_lcd_sim_input_get(esp_lcd_panel_handle_t panel, esp_lcd_sim_input_data_t *data);

/**
 * @brief Inject a programmatic touch state into a panel
 *
 * Sets the pointer state that esp_lcd_sim_input_get() reports for this panel,
 * replacing the preview window's real mouse. Sticky: kept until the next
 * inject call or esp_lcd_sim_input_inject_reset(), so a polling GUI input
 * device sees a stable press or release. A state sample, not an event queue:
 * script a click as press, let the GUI run, release. Works with or without a
 * preview window; the injected pointer is not drawn into the preview. While
 * injection is active the real mouse is ignored for this panel.
 * `pressed = false` is a lifted finger at (x, y) and keeps the override
 * installed; only esp_lcd_sim_input_inject_reset() hands control back to the
 * mouse.
 *
 * @note Thread-safe: may be called from any thread while the GUI thread draws.
 *
 * @param[in] panel   Panel handle created by esp_lcd_new_panel_sim_sdl()
 * @param[in] x       Touch position in panel pixels, horizontal
 * @param[in] y       Touch position in panel pixels, vertical
 * @param[in] pressed true for finger/button down, false for up
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if panel is NULL or is not a SDL sim panel
 */
esp_err_t esp_lcd_sim_input_inject_touch(esp_lcd_panel_handle_t panel, int x, int y, bool pressed);

/**
 * @brief Stop injecting touch and return to the real pointer
 *
 * Clears the override installed by esp_lcd_sim_input_inject_touch();
 * esp_lcd_sim_input_get() samples the preview window's mouse again.
 *
 * @note Thread-safe: may be called from any thread.
 *
 * @param[in] panel Panel handle created by esp_lcd_new_panel_sim_sdl()
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if panel is NULL or is not a SDL sim panel
 */
esp_err_t esp_lcd_sim_input_inject_reset(esp_lcd_panel_handle_t panel);

/**
 * @brief Event callbacks of the simulated panel
 *
 * `on_flush_done` is the analog of `on_color_trans_done` of a transfer-based
 * display driver: it fires synchronously after each draw_bitmap() has copied
 * its pixels into the framebuffer — once per draw call, not once per frame.
 * A GUI port calls lv_display_flush_ready() from it. It runs on the GUI thread
 * that triggered the draw, outside the panel mutex (it may block), and must
 * not call back into the drawing functions of the same panel.
 */
typedef struct {
    void (*on_flush_done)(esp_lcd_panel_handle_t panel, void *user_ctx); /*!< After each draw_bitmap reached the framebuffer */
} esp_lcd_sim_event_callbacks_t;

/**
 * @brief Register event callbacks of a simulated panel
 *
 * The callbacks are copied into the panel, so the structure can be a
 * temporary. Pass NULL as `callbacks` to unregister. Safe to call at any time
 * while the panel is alive; register before threads start drawing.
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
 * Exports one consistent frame at the configured resolution and color format,
 * independent of the preview scaling and the board frame. The framebuffer is
 * snapshotted under the panel lock and encoded outside it, so drawing is not
 * paused. Always writes the exact path given, overwriting an existing file
 * (the interactive screenshot key never overwrites instead).
 *
 * @note Thread-safe: may be called from any thread (for example a console
 *       REPL task) while the GUI thread draws.
 *
 * @param[in] panel    Panel handle created by esp_lcd_new_panel_sim_sdl()
 * @param[in] filepath Path of the PNG file to write
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if filepath is NULL, or panel is NULL or is not a SDL sim panel
 *      - ESP_ERR_NO_MEM if memory allocation for the framebuffer snapshot, the PNG encoder or the scanline buffer fails
 *      - ESP_FAIL if filepath cannot be opened or the PNG file cannot be written
 */
esp_err_t esp_lcd_sim_screenshot_save_png(esp_lcd_panel_handle_t panel, const char *filepath);

/**
 * @brief Console command interface configuration
 *
 * Copied by esp_lcd_sim_console_register(); the `quit_requested` pointer it
 * stores is kept by the component and must outlive the registration and the
 * REPL task that runs the commands.
 */
typedef struct {
    esp_lcd_panel_handle_t panel; /*!< Panel the commands operate on, created by esp_lcd_new_panel_sim_sdl() */
    uint32_t touch_settle_ms;     /*!< Sleep after each `touch` so the GUI thread samples the input and finishes
                                       the gesture before the next command, 0 selects a 100 ms default */
    bool *quit_requested;         /*!< `quit` sets *quit_requested = true so the application loop can stop;
                                       NULL makes `quit` a no-op */
} esp_lcd_sim_console_config_t;

/**
 * @brief Register the esp_console command set of a simulated panel
 *
 * Registers a fixed set of esp_console commands that drive `config->panel`
 * from a command line, so a host test (pytest) can script touch input and
 * frame export. The command names and arguments are part of the public API
 * of this component:
 *
 * ```text
 * touch <x> <y> <0|1>   esp_lcd_sim_input_inject_touch(), then sleep touch_settle_ms
 * reset                 esp_lcd_sim_input_inject_reset()
 * frame <path>          esp_lcd_sim_screenshot_save_png()
 * wait <ms>             sleep <ms> while the GUI runs without input (capped at 10000 ms)
 * quit                  set *quit_requested
 * help                  list the commands (built-in esp_console help)
 * ```
 *
 * The commands run on a background REPL task (created by the application
 * with esp_console_new_repl_stdio() + esp_console_start_repl()) while the GUI
 * renders on its own thread; they only call the thread-safe panel entry
 * points, never SDL. `touch` and `wait` sleep so the independent GUI thread
 * can sample the input and advance before the next command runs. Requires
 * CONFIG_ESP_LCD_SIM_CONSOLE. The set drives a single panel; register it once
 * after the REPL is created. Stop the REPL and the GUI loop before
 * esp_lcd_panel_del(). The programming guide shows the application layout.
 *
 * @param[in] config Command interface configuration
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_ARG if config or config->panel is NULL
 *      - ESP_ERR_INVALID_STATE if the command set is already registered
 *      - ESP_ERR_NOT_SUPPORTED if CONFIG_ESP_LCD_SIM_CONSOLE is disabled
 *      - other error codes from esp_console_cmd_register()
 */
esp_err_t esp_lcd_sim_console_register(const esp_lcd_sim_console_config_t *config);

#ifdef __cplusplus
}
#endif
