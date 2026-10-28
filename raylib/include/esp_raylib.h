/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle to a display registered with esp_raylib_add_display().
 */
typedef struct esp_raylib_display_t *esp_raylib_display_handle_t;

/**
 * @brief Deliver a finished frame to the panel.
 *
 * @param display  The display delivering the frame -- the handle returned by
 *                 esp_raylib_add_display(). It identifies which display is calling.
 * @param pixels  w*h pixels in CONFIG_RAYLIB_FRAMEBUFFER_FORMAT, i.e. w*h*bpp
 *                bytes (bpp = 1, 2, 3 or 4 per the selected format).
 * @param x,y     Top-left corner in panel coordinates
 * @param w,h     Rectangle size
 * @param user_data  Opaque context from esp_raylib_display_config_t, passed
 *                   back untouched on every call
 *
 * @note Return only after the pixels have been consumed: the buffer belongs to the
 * component and is overwritten by the next band. esp_lcd_panel_draw_bitmap()
 * queues its DMA and returns early, so wait for its trans-done event or copy the pixels first.
 */
typedef void (*esp_raylib_flush_cb_t)(esp_raylib_display_handle_t display, const void *pixels,
                                      uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      void *user_data);

/**
 * @brief How a display's frames are delivered to its panel.
 *
 * flush_cb is required and cannot be changed after registration.
 * When chunk_rows is set, raylib calls it once per horizontal band instead of once
 * per frame; bands keep the DMA-visible buffer small enough to live in internal RAM.
 */
typedef struct {
    uint16_t chunk_rows;            /**< Rows per band; 0 sends the whole frame at once */
    bool swap_bytes;                /**< Swap the two bytes of each pixel; RGB565 only */
    esp_raylib_flush_cb_t flush_cb; /**< Receives one band, or the whole frame; required */
    void *user_data;                /**< Opaque context, passed back to flush_cb untouched */
} esp_raylib_display_config_t;

/**
 * @brief Register a display for raylib to render to, and return its handle.
 *
 * Call before InitWindow(). The component never sees the esp_lcd panel: create
 * and initialise it yourself and let flush_cb reach it through user_data.
 * Only one display is currently supported: a second call returns NULL.
 *
 * @param config Raylib platform display driver configuration
 * @return An opaque display handle, or NULL on a bad config or when a display has already been added.
 */
esp_raylib_display_handle_t esp_raylib_add_display(const esp_raylib_display_config_t *config);

#ifdef __cplusplus
}
#endif
