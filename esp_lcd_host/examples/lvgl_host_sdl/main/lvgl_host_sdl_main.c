/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * Render an LVGL screen into the SDL backed esp_lcd panel of esp_lcd_host,
 * which stands in for a real LCD. The example is written the way a real GUI
 * port would be: it only calls the generic esp_lcd panel API and it does not
 * know whether the panel is the SDL host panel or a real display controller.
 *
 * The application renders once, saves the frame as a PNG file and exits. The
 * example builds and runs on the host, so the pytest script simply reads the
 * PNG file and compares it with the committed golden image.
 */

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"

#include "esp_lcd_host.h"

#define EXAMPLE_LCD_H_RES       240
#define EXAMPLE_LCD_V_RES       240
#define EXAMPLE_DRAW_BUF_LINES  40
#define EXAMPLE_PNG_PATH        "screenshot.png"
/* The frame is rendered once, so the application stops after this many
 * esp_lcd_host_pump_events() calls. */
#define EXAMPLE_PUMP_FRAMES     5
#define EXAMPLE_PUMP_PERIOD_MS  100

static const char *TAG = "example";

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

    /* Tell LVGL that it may reuse the draw buffer for the next render. */
    lv_display_flush_ready(display);
}

/* A static screen: only the widget values matter, nothing depends on time, so
 * the rendered frame (and the test golden image) is deterministic. */
static void example_create_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x103a5c), LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "esp_lcd_host");
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
}

void app_main(void)
{
    ESP_LOGI(TAG, "Install the SDL host LCD panel driver");
    esp_lcd_host_config_t panel_config = {
        .width = EXAMPLE_LCD_H_RES,
        .height = EXAMPLE_LCD_V_RES,
        /* LVGL's RGB888 buffer is stored as B, G, R in memory. */
        .color_format = ESP_COLOR_FOURCC_BGR24,
        /* Set to false on a machine without a display, for example in CI. */
        .create_window = true,
        .scale = 2,
        .window_title = "esp_lcd_host example",
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_host_sdl(&panel_config, &panel));

    /* A GUI port would place these calls in board_init() and switch to
     * esp_lcd_new_panel_host_sdl() only for the host build. */
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Register display driver to LVGL");
    lv_display_t *display = lv_display_create(EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    /* This must match panel_config.color_format and the draw buffer layout. */
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB888);
    lv_display_set_user_data(display, panel);
    lv_display_set_flush_cb(display, example_lvgl_flush_cb);

    static uint8_t draw_buf[EXAMPLE_LCD_H_RES * EXAMPLE_DRAW_BUF_LINES * sizeof(lv_color_t)];
    lv_display_set_buffers(display, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);

    ESP_LOGI(TAG, "Create a static demo UI and render it once");
    example_create_ui();
    /* Force an immediate refresh instead of waiting for an LVGL timer tick. */
    lv_refr_now(display);

    /* This is an application and not an LVGL port with a task loop, so pump the
     * preview window from here until the frame is on screen. */
    for (int i = 0; i < EXAMPLE_PUMP_FRAMES; i++) {
        ESP_ERROR_CHECK(esp_lcd_host_pump_events());
        lv_delay_ms(EXAMPLE_PUMP_PERIOD_MS);
    }

    ESP_LOGI(TAG, "Save the rendered frame as a PNG file");
    /* The PNG uses the panel resolution and color format, so a scaling preview
     * window does not change it. */
    ESP_ERROR_CHECK(esp_lcd_host_screenshot_save_png(panel, EXAMPLE_PNG_PATH));

    ESP_ERROR_CHECK(esp_lcd_panel_del(panel));
    ESP_LOGI(TAG, "LVGL host SDL example done.");
}
