/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * Render an LVGL screen into an SDL backed esp_lcd panel, which stands in for a
 * real LCD. The example is written the way a real GUI port would be: it only
 * calls the generic esp_lcd panel API and it does not know whether the panel is
 * the SDL host panel or a real display controller.
 *
 * The rendered frame is saved as a PNG file, which the pytest script checks and
 * compares with a committed golden image.
 *
 * The reference framebuffer below is filled with the same LVGL flush callback
 * that feeds the panel. It is exported as an RGB888 PPM file next to the PNG, so
 * the very same frame can be inspected without decoding a PNG.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_check.h"
#include "esp_log.h"
#include "lvgl.h"

#include "esp_lcd_host.h"

#define EXAMPLE_LCD_H_RES       240
#define EXAMPLE_LCD_V_RES       240
#define EXAMPLE_DRAW_BUF_LINES  40
#define EXAMPLE_PNG_PATH        "screenshot.png"
#define EXAMPLE_PPM_PATH        "lvgl_host_sdl_result.ppm"

static const char *TAG = "example";

/* Independent copy of everything the GUI rendered. */
static uint8_t reference_framebuffer[EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * 3];

/*
 * LVGL calls this function whenever a part of the screen is ready to be
 * displayed. The panel is a regular esp_lcd panel, so the draw buffer is passed
 * to it exactly like it would be passed to a physical LCD driver.
 */
static void example_lvgl_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    /* The panel handle is stored in LVGL's user-data field below. */
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(display);

    /* LVGL's area uses inclusive x2/y2 coordinates, while esp_lcd expects an
     * exclusive right/bottom coordinate. */
    const int width = area->x2 - area->x1 + 1;
    const int height = area->y2 - area->y1 + 1;
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);

    /* Keep the reference image in sync with the panel. LVGL RGB888 is stored as
     * B, G, R in memory, which is the BGR24 layout used by the panel. */
    for (int y = 0; y < height; y++) {
        const uint8_t *src = px_map + (size_t)y * width * 3;
        uint8_t *dst = reference_framebuffer + (((size_t)(area->y1 + y) * EXAMPLE_LCD_H_RES) + area->x1) * 3;
        memcpy(dst, src, (size_t)width * 3);
    }

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

/* Write a binary PPM (P6) file with the reference image, so the very same frame
 * can be inspected on the host without decoding the PNG. */
static esp_err_t example_save_ppm(const char *filepath)
{
    FILE *f = fopen(filepath, "wb");
    ESP_RETURN_ON_FALSE(f, ESP_FAIL, TAG, "failed to open '%s'", filepath);
    fprintf(f, "P6\n%d %d\n255\n", EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    /* LVGL RGB888 is stored as B, G, R; PPM expects R, G, B. */
    uint8_t *rgb = malloc(sizeof(reference_framebuffer));
    if (rgb) {
        for (int i = 0; i < EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES; i++) {
            rgb[i * 3 + 0] = reference_framebuffer[i * 3 + 2];
            rgb[i * 3 + 1] = reference_framebuffer[i * 3 + 1];
            rgb[i * 3 + 2] = reference_framebuffer[i * 3 + 0];
        }
        fwrite(rgb, 1, sizeof(reference_framebuffer), f);
        free(rgb);
    }
    fclose(f);
    ESP_LOGI(TAG, "Reference frame written to %s", filepath);
    return ESP_OK;
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
        .use_renderer = true,
        .scale = 2,
        .window_title = "esp_lcd_host example",
    };
    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_host_sdl(&panel_config, &panel_handle));

    /* A GUI port would place these calls in board_init() and switch to
     * esp_lcd_new_panel_host_sdl() only for the host build. */
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    esp_lcd_host_target_t target = ESP_LCD_HOST_TARGET_ESP32;
    ESP_ERROR_CHECK(esp_lcd_host_get_target(panel_handle, &target));
    ESP_LOGI(TAG, "Panel simulator running on %s",
             target == ESP_LCD_HOST_TARGET_POSIX ? "the POSIX host target" : "an ESP32 chip");

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Register display driver to LVGL");
    lv_display_t *display = lv_display_create(EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    /* This must match panel_config.color_format and the draw buffer layout. */
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB888);
    lv_display_set_user_data(display, panel_handle);
    lv_display_set_flush_cb(display, example_lvgl_flush_cb);

    static uint8_t draw_buf[EXAMPLE_LCD_H_RES * EXAMPLE_DRAW_BUF_LINES * sizeof(lv_color_t)];
    lv_display_set_buffers(display, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);

    ESP_LOGI(TAG, "Create a static demo UI and render it once");
    example_create_ui();
    /* Force an immediate refresh instead of waiting for an LVGL timer tick. */
    lv_refr_now(display);

    /* Push the frame to the preview window, if one was created. The pixel data
     * is complete now, but before the check make sure the frame really reached
     * the panel. */
    ESP_ERROR_CHECK(esp_lcd_host_pump_events());

    ESP_LOGI(TAG, "Save the rendered frame as a PNG file");
    /* The PNG uses the panel resolution and color format, so a scaling preview
     * window does not change it. */
    ESP_ERROR_CHECK(esp_lcd_host_screenshot_save_png(panel_handle, EXAMPLE_PNG_PATH));
    ESP_ERROR_CHECK(example_save_ppm(EXAMPLE_PPM_PATH));

    ESP_ERROR_CHECK(esp_lcd_host_return_panel(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_host_return_buffers(panel_handle));

    ESP_LOGI(TAG, "LVGL host SDL example done.");
}
