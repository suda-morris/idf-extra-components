/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * Render an LVGL screen into the SDL backed esp_lcd panel of esp_lcd_sim,
 * which stands in for a real LCD. The example is written the way a real GUI
 * port would be: it only calls the generic esp_lcd panel API and it does not
 * know whether the panel is the SDL sim panel or a real display controller.
 *
 * Two threads cooperate, the way a real application does:
 *   - this thread (app_main) is the GUI thread. It owns the SDL preview and
 *     runs LVGL: the flush callback draws into the panel and the pointer indev
 *     polls esp_lcd_sim_input_get().
 *   - a background console REPL task (esp_console_new_repl_stdio +
 *     esp_console_start_repl) runs the component's command set
 *     (esp_lcd_sim_console_register) plus the application's own commands. A
 *     host test (pytest) writes command lines to stdin to inject touches and
 *     export frames while the GUI keeps rendering.
 *
 * The two threads share the panel only through its thread-safe entry points
 * (inject_touch, inject_reset, screenshot_save_png); the GUI thread alone
 * touches SDL. On 'quit' (or a window close) the GUI loop stops, the frame is
 * saved as a PNG and the application exits.
 */

#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sim.h"

#define EXAMPLE_LCD_H_RES       240
#define EXAMPLE_LCD_V_RES       320
#define EXAMPLE_DRAW_BUF_LINES  40
#define EXAMPLE_PNG_PATH        "screenshot.png"
/* Wall time given to the GUI after each injected touch so the input device
 * samples the new state and click/scroll gestures complete. */
#define EXAMPLE_TOUCH_SETTLE_MS 100
/* Headless the idle wait is a sleep that an injected touch cannot interrupt,
 * so the loop caps it to keep input responsive even when LVGL reports a long
 * idle. With a window the wait ends on the next preview event instead. */
#define EXAMPLE_IDLE_SLICE_MS   20

static const char *TAG = "example";

/* LVGL needs a time source. On a chip it comes from a hardware timer; on the
 * host build the monotonic esp_timer provides it. */
static uint32_t example_tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* lv_delay_ms() busy-waits on the tick by default; hand it a real sleep so the
 * waiting task yields to the other tasks of the system. */
static void example_delay_cb(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/*
 * LVGL calls this function whenever a part of the screen is ready to be
 * displayed. The panel is a regular esp_lcd panel, so the draw buffer is passed
 * to it exactly like it would be passed to a physical LCD driver.
 */
static void example_lvgl_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(display);

    /* LVGL's area uses inclusive x2/y2 coordinates, while esp_lcd expects an
     * exclusive right/bottom coordinate. */
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
}

/*
 * The sim panel reports flush completion the same way a real transfer-based
 * driver does: through the registered event callback. On hardware this is
 * where the color transfer done (DMA) callback calls lv_display_flush_ready();
 * the sim's copy is synchronous, so the callback fires inside the draw call
 * above, before it returns.
 */
static void example_on_flush_done(esp_lcd_panel_handle_t panel, void *user_ctx)
{
    lv_display_flush_ready((lv_display_t *)user_ctx);
}

/* Pointer feedback: the label starts out empty (an empty label paints
 * nothing, so the initial frame and the test golden image stay unchanged),
 * follows the hover of the pointer indev and counts the clicks. */
static void example_button_event(lv_event_t *event)
{
    lv_obj_t *label = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_CLICKED) {
        static int s_clicks = 0;
        s_clicks++;
        lv_label_set_text_fmt(label, "clicked %d times", s_clicks);
    } else if (code == LV_EVENT_HOVER_OVER) {
        lv_label_set_text(label, "hovering");
    } else if (code == LV_EVENT_HOVER_LEAVE) {
        lv_label_set_text(label, "");
    }
}

/* A static screen: only the widget values matter, nothing depends on time, so
 * the rendered frame (and the test golden image) is deterministic. */
static void example_create_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x103a5c), LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "esp_lcd_sim");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *chart = lv_chart_create(scr);
    lv_obj_set_size(chart, 200, 90);
    lv_obj_align(chart, LV_ALIGN_TOP_MID, 0, 40);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_series_t *series = lv_chart_add_series(chart, lv_color_hex(0x5ce08a), LV_CHART_AXIS_PRIMARY_Y);
    for (int i = 0; i < 10; i++) {
        lv_chart_set_next_value(chart, series, 20 + (i * i) % 60);
    }
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);

    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 120, 50);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 55);
    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Press me");
    lv_obj_center(btn_label);

    lv_obj_t *click_label = lv_label_create(scr);
    /* lv_label_create() defaults to the placeholder "Text" when
     * LV_WIDGETS_HAS_DEFAULT_VALUE is enabled; clear it so the label really
     * paints nothing until the pointer interacts with the button. */
    lv_label_set_text(click_label, "");
    lv_obj_set_style_text_color(click_label, lv_color_hex(0xffffff), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(click_label, LV_ALIGN_CENTER, 0, 100);
    lv_obj_add_event_cb(btn, example_button_event, LV_EVENT_ALL, click_label);
}

/*
 * The host mouse is the touch input of the simulated display. LVGL polls this
 * callback from its input device timer; the panel translates the pointer of
 * the preview window into panel pixels (see esp_lcd_sim_input_get()). When a
 * test has injected a touch, input_get reports that instead, so the same GUI
 * is drivable headless.
 */
static void example_mouse_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    esp_lcd_panel_handle_t panel = lv_indev_get_user_data(indev);
    esp_lcd_sim_input_data_t mouse = {0};
    if (esp_lcd_sim_input_get(panel, &mouse) == ESP_OK) {
        data->point.x = mouse.x;
        data->point.y = mouse.y;
        data->state = mouse.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    }
}

/* An application command unrelated to the LCD, registered on the same REPL as
 * the component's command set. It shows the two coexist: the REPL task runs
 * both while the GUI renders on the main thread. */
static int example_hello_cmd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("Hello from the example: the GUI keeps rendering on its own thread.\n");
    return 0;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Install the SDL simulated LCD panel driver");
    esp_lcd_sim_config_t panel_config = {
        .width = EXAMPLE_LCD_H_RES,
        .height = EXAMPLE_LCD_V_RES,
        /* LVGL's RGB888 buffer is stored as B, G, R in memory. */
        .color_format = ESP_COLOR_FOURCC_BGR24,
        /* A preview window needs a display. The example is also run on a
         * headless machine (the CI), so a failure to open the window is not
         * fatal: the panel falls back to a framebuffer that still renders and
         * exports the frame. Set this to false to never open a window. */
        .create_window = true,
        /* Show the LVGL content inside a screen drawn on a development-board
         * frame. The exported PNG still contains only the 240x320 LCD. */
        .show_board_frame = true,
        .scale = 1,
        .window_title = "LVGL Example Simulator",
        /* Press S in the preview window to write a PNG without stopping. */
        .screenshot_key = 's',
        .screenshot_path = EXAMPLE_PNG_PATH,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_sim_sdl(&panel_config, &panel));
    /* On a machine without a display the panel fell back to framebuffer-only
     * (with a warning from the component); the render and the PNG export work
     * either way. */
    bool with_window = esp_lcd_sim_window_is_open(panel);
    if (!with_window) {
        ESP_LOGW(TAG, "No preview window available, continue with a framebuffer only panel");
    }

    /* A GUI port would place these calls in board_init() and switch to
     * esp_lcd_new_panel_sim_sdl() only for the host build. The sim panel
     * accepts the hardware programming ops (reset, mirror, ...) as no-ops,
     * so the ESP_ERROR_CHECK() wrappers of a real port are safe here. */
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();
    lv_tick_set_cb(example_tick_cb);
    lv_delay_set_cb(example_delay_cb);

    ESP_LOGI(TAG, "Register display driver to LVGL");
    lv_display_t *display = lv_display_create(EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    /* This must match panel_config.color_format and the draw buffer layout. */
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB888);
    lv_display_set_user_data(display, panel);
    lv_display_set_flush_cb(display, example_lvgl_flush_cb);

    static uint8_t draw_buf[EXAMPLE_LCD_H_RES * EXAMPLE_DRAW_BUF_LINES * sizeof(lv_color_t)];
    lv_display_set_buffers(display, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);

    /* LVGL's flush completion comes through the panel event callback, the
     * same structure a GUI port uses with a real transfer-based driver. */
    ESP_LOGI(TAG, "Register panel event callbacks");
    ESP_ERROR_CHECK(esp_lcd_sim_register_event_callbacks(panel,
                                                         &(esp_lcd_sim_event_callbacks_t) {
                                                             .on_flush_done = example_on_flush_done,
                                                         },
                                                         display));

    /* The pointer indev polls esp_lcd_sim_input_get() through the read
     * callback below. With a preview window it follows the real mouse; without
     * one it reports released at (0,0) until a test injects a touch with
     * esp_lcd_sim_input_inject_touch(), so the same GUI is drivable headless. */
    ESP_LOGI(TAG, "Register the pointer input device");
    lv_indev_t *mouse_indev = lv_indev_create();
    lv_indev_set_type(mouse_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(mouse_indev, display);
    lv_indev_set_user_data(mouse_indev, panel);
    lv_indev_set_read_cb(mouse_indev, example_mouse_read_cb);

    ESP_LOGI(TAG, "Create a static demo UI and render it once");
    example_create_ui();
    /* Force an immediate refresh instead of waiting for an LVGL timer tick. */
    lv_refr_now(display);

    /* Start a background console REPL on its own task. The component's command
     * set (touch/reset/frame/wait/quit) and the application's own commands run
     * there, while this thread keeps rendering the GUI. esp_console_new_repl_stdio
     * initializes esp_console and spawns the task; start_repl lets it run. */
    bool quit = false;
    const esp_lcd_sim_console_config_t console_config = {
        .panel = panel,
        .touch_settle_ms = EXAMPLE_TOUCH_SETTLE_MS,
        .quit_requested = &quit,
    };
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "lvgl_sim>";
    esp_console_repl_t *repl = NULL;
    ESP_ERROR_CHECK(esp_console_new_repl_stdio(&repl_config, &repl));
    ESP_ERROR_CHECK(esp_lcd_sim_console_register(&console_config));
    /* An application command unrelated to the LCD, to show the two coexist. */
    const esp_console_cmd_t hello_cmd = {
        .command = "hello",
        .help = "Print a message (an application command, unrelated to the LCD)",
        .func = &example_hello_cmd,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&hello_cmd));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));

    /* This thread is the GUI thread: it owns the SDL preview and runs LVGL.
     * Each iteration pumps the preview events, runs the LVGL timers and idles
     * until input or the next timer, so an idle screen costs almost no CPU.
     * With a window the loop also ends when the user closes it; headless there
     * is no window to close, so only 'quit' ends it while the REPL task feeds
     * it injected touches. */
    ESP_LOGI(TAG, "Run the GUI loop (close the window or type 'quit' to stop)");
    while (!quit && (!with_window || esp_lcd_sim_window_is_open(panel))) {
        ESP_ERROR_CHECK(esp_lcd_sim_pump_events());
        uint32_t idle_ms = lv_timer_handler();
        if (!with_window) {
            idle_ms = idle_ms > EXAMPLE_IDLE_SLICE_MS ? EXAMPLE_IDLE_SLICE_MS : idle_ms;
        }
        /* The same idle call in both modes: with a window it returns on the
         * next preview event, headless it sleeps the bounded timeout. */
        ESP_ERROR_CHECK(esp_lcd_sim_wait_events(idle_ms));
    }

    ESP_LOGI(TAG, "Save the rendered frame as a PNG file");
    /* The PNG uses the panel resolution and color format, so a scaling preview
     * window does not change it. */
    ESP_ERROR_CHECK(esp_lcd_sim_screenshot_save_png(panel, EXAMPLE_PNG_PATH));

    /* Stop the REPL task before tearing the panel down: it waits for the task
     * to exit and frees the REPL, so no command can run against a deleted panel
     * afterwards. */
    ESP_ERROR_CHECK(esp_console_stop_repl(repl));
    ESP_ERROR_CHECK(esp_lcd_panel_del(panel));
    /* Release the SDL video subsystem the component initialized for the
     * preview window. Every panel has to be deleted first. */
    ESP_ERROR_CHECK(esp_lcd_sim_deinit());
    ESP_LOGI(TAG, "LVGL sim SDL example done.");
}
