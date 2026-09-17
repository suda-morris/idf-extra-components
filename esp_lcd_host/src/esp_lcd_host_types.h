/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Definitions the panel driver needs from the generic `esp_lcd` component.
 *
 * The `esp_lcd` component does not build for the ESP-IDF host (POSIX) target,
 * because it depends on chip peripherals. The public API of this component is
 * still the standard `esp_lcd` panel interface, so on the host target the
 * handful of declarations that describe a panel handle are provided here, with
 * the layout used by ESP-IDF. On every chip target the real headers are used.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

/* The HAL provides the color fourcc definitions on every target, including the
 * POSIX host. */
#include "hal/color_types.h"

#if !defined(ESP_COLOR_FOURCC_RGB16)
#error "hal/color_types.h does not provide the color fourcc definitions"
#endif

#if __has_include("esp_lcd_types.h")
#include "esp_lcd_types.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_ops.h"
#define ESP_LCD_HOST_HAS_ESP_LCD 1
#else

typedef struct esp_lcd_panel_t esp_lcd_panel_t;
typedef esp_lcd_panel_t *esp_lcd_panel_handle_t;

struct esp_lcd_panel_t {
    esp_err_t (*reset)(esp_lcd_panel_t *panel);
    esp_err_t (*init)(esp_lcd_panel_t *panel);
    esp_err_t (*draw_bitmap)(esp_lcd_panel_t *panel, int x_start, int y_start,
                             int x_end, int y_end, const void *color_data);
    esp_err_t (*mirror)(esp_lcd_panel_t *panel, bool x_axis, bool y_axis);
    esp_err_t (*swap_xy)(esp_lcd_panel_t *panel, bool swap_axes);
    esp_err_t (*set_gap)(esp_lcd_panel_t *panel, int x_gap, int y_gap);
    esp_err_t (*invert_color)(esp_lcd_panel_t *panel, bool invert_color_data);
    esp_err_t (*disp_on_off)(esp_lcd_panel_t *panel, bool on_off);
    esp_err_t (*disp_sleep)(esp_lcd_panel_t *panel, bool sleep);
    esp_err_t (*draw_bitmap_2d)(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end,
                                const void *src_data, size_t src_x_size, size_t src_y_size,
                                int src_x_start, int src_y_start, int src_x_end, int src_y_end);
    esp_err_t (*set_brightness)(esp_lcd_panel_t *panel, int brightness);
    esp_err_t (*del)(esp_lcd_panel_t *panel);
};

#endif /* __has_include("esp_lcd_types.h") */

/*
 * The generic panel operations of esp_lcd_panel_ops.h. They are declared here on
 * the host target so that an application and the tests can use the same calls as
 * on a chip. They dispatch to the callbacks of the panel handle, exactly like
 * the real implementation does.
 */
esp_err_t esp_lcd_panel_reset(esp_lcd_panel_handle_t panel);
esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t panel);
esp_err_t esp_lcd_panel_del(esp_lcd_panel_handle_t panel);
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x_start, int y_start,
                                    int x_end, int y_end, const void *color_data);
esp_err_t esp_lcd_panel_draw_bitmap_2d(esp_lcd_panel_handle_t panel, int x_start, int y_start,
                                       int x_end, int y_end, const void *color_data,
                                       size_t src_x_size, size_t src_y_size,
                                       int src_x_start, int src_y_start, int src_x_end, int src_y_end);
esp_err_t esp_lcd_panel_mirror(esp_lcd_panel_handle_t panel, bool mirror_x, bool mirror_y);
esp_err_t esp_lcd_panel_swap_xy(esp_lcd_panel_handle_t panel, bool swap_axes);
esp_err_t esp_lcd_panel_set_gap(esp_lcd_panel_handle_t panel, int x_gap, int y_gap);
esp_err_t esp_lcd_panel_invert_color(esp_lcd_panel_handle_t panel, bool invert_color_data);
esp_err_t esp_lcd_panel_disp_on_off(esp_lcd_panel_handle_t panel, bool on_off);
esp_err_t esp_lcd_panel_disp_sleep(esp_lcd_panel_handle_t panel, bool sleep);
