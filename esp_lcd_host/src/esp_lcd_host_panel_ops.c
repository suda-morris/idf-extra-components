/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host fallback of the generic panel operations.
 *
 * The real implementations live in the `esp_lcd` component, which cannot build
 * for the POSIX host target. This file is only compiled for that target, so an
 * application and the tests can call the same `esp_lcd_panel_*()` functions as
 * they would on a chip.
 */
#include "esp_lcd_host_types.h"

#ifndef ESP_LCD_HOST_HAS_ESP_LCD

/* reset() and init() are optional in the panel interface: a panel that has
 * nothing to do during the lifecycle steps simply leaves the callback NULL, and
 * the generic API reports success. del() and the operations that change how the
 * panel is displayed report ESP_ERR_NOT_SUPPORTED instead. */
esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t panel)
{
    if (!panel) {
        return ESP_ERR_INVALID_ARG;
    }
    return panel->reset ? panel->reset(panel) : ESP_OK;
}

esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t panel)
{
    if (!panel) {
        return ESP_ERR_INVALID_ARG;
    }
    return panel->init ? panel->init(panel) : ESP_OK;
}

esp_err_t esp_lcd_panel_del(esp_lcd_panel_handle_t panel)
{
    if (!panel || !panel->del) {
        return ESP_ERR_INVALID_ARG;
    }
    return panel->del(panel);
}

esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x_start, int y_start,
                                    int x_end, int y_end, const void *color_data)
{
    if (!panel || !color_data || x_start >= x_end || y_start >= y_end) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!panel->draw_bitmap) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return panel->draw_bitmap(panel, x_start, y_start, x_end, y_end, color_data);
}

esp_err_t esp_lcd_panel_draw_bitmap_2d(esp_lcd_panel_handle_t panel, int x_start, int y_start,
                                       int x_end, int y_end, const void *color_data,
                                       size_t src_x_size, size_t src_y_size,
                                       int src_x_start, int src_y_start, int src_x_end, int src_y_end)
{
    if (!panel || !color_data || x_start >= x_end || y_start >= y_end) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!panel->draw_bitmap_2d) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return panel->draw_bitmap_2d(panel, x_start, y_start, x_end, y_end, color_data, src_x_size, src_y_size,
                                 src_x_start, src_y_start, src_x_end, src_y_end);
}

esp_err_t esp_lcd_panel_mirror(esp_lcd_panel_handle_t panel, bool mirror_x, bool mirror_y)
{
    if (!panel) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!panel->mirror) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return panel->mirror(panel, mirror_x, mirror_y);
}

esp_err_t esp_lcd_panel_swap_xy(esp_lcd_panel_handle_t panel, bool swap_axes)
{
    if (!panel) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!panel->swap_xy) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return panel->swap_xy(panel, swap_axes);
}

esp_err_t esp_lcd_panel_set_gap(esp_lcd_panel_handle_t panel, int x_gap, int y_gap)
{
    if (!panel) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!panel->set_gap) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return panel->set_gap(panel, x_gap, y_gap);
}

esp_err_t esp_lcd_panel_invert_color(esp_lcd_panel_handle_t panel, bool invert_color_data)
{
    if (!panel) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!panel->invert_color) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return panel->invert_color(panel, invert_color_data);
}

esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t panel, bool on_off)
{
    if (!panel) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!panel->disp_on_off) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return panel->disp_on_off(panel, on_off);
}

esp_err_t esp_lcd_panel_disp_sleep(esp_lcd_panel_handle_t panel, bool sleep)
{
    if (!panel) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!panel->disp_sleep) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return panel->disp_sleep(panel, sleep);
}

#endif /* !ESP_LCD_HOST_HAS_ESP_LCD */
