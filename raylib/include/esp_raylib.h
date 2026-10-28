/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * ESP-IDF integration for raylib.
 *
 * The upstream raylib API is unchanged: keep writing InitWindow()/BeginDrawing()
 * /EndDrawing(). This header only adds the glue that hands finished frames to an
 * esp_lcd panel.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque reference to a display.
 *
 * Usually the esp_lcd panel handle; raylib only passes it back to the flush
 * callback.
 */
typedef void *esp_raylib_display_handle_t;

/**
 * @brief Deliver a finished frame to the panel.
 *
 * @param handle  Handle passed in the display config
 * @param pixels  Pixels for the rectangle, one of the layouts below
 * @param x,y     Top-left corner in panel coordinates
 * @param w,h     Rectangle size
 *
 * Pixel layout follows CONFIG_RAYLIB_FRAMEBUFFER_FORMAT, which is also what
 * raylib's software renderer draws in:
 *
 *   RGB565    two bytes, R in bits 15:11, G in 10:5, B in 4:0
 *   RGB888    three bytes, R then G then B
 *   RGBA8888  four bytes, R then G then B then A
 *   RGB332    one byte, R in bits 7:5, G in 4:2, B in 1:0
 *
 * A byte swap of the 16 bit words is available for SPI and DPI panels that take
 * RGB565 MSB first; see esp_raylib_display_config_t::swap_bytes.
 *
 * Return only after the pixels have been consumed: the buffer belongs to the
 * component and is overwritten by the next band. esp_lcd_panel_draw_bitmap()
 * queues its DMA and returns early, so wait for its trans-done event or copy
 * the pixels first.
 *
 * Call it from the component only: frames are presented by the component's
 * internal present task, one band at a time, never interleaved.
 */
typedef void (*esp_raylib_flush_cb_t)(esp_raylib_display_handle_t handle, const uint16_t *pixels,
                                      uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/**
 * @brief Optional per-band transfer for SPI panels.
 *
 * When set, raylib calls flush_cb once per horizontal band of chunk_rows lines
 * instead of once per frame. Bands keep the DMA-visible buffer small enough to
 * live in internal RAM; the swap/flip transform is still done for you.
 *
 * Fields left at zero keep their neutral meaning: no byte swap, no flip, and a
 * whole-frame transfer.
 */
typedef struct {
    esp_raylib_display_handle_t handle; /**< Passed back to the callbacks */
    uint16_t width;                     /**< Panel width in pixels, has to match InitWindow() */
    uint16_t height;                    /**< Panel height in pixels, has to match InitWindow() */
    bool swap_bytes;                    /**< Swap the two bytes of each pixel; RGB565 only */
    bool invert;                        /**< Flip vertically (panel origin at bottom) */
    uint16_t chunk_rows;                /**< Rows per band; 0 sends the whole frame at once */
    esp_raylib_flush_cb_t flush_cb;     /**< Receives one band, or the whole frame */
} esp_raylib_display_config_t;

/**
 * @brief Register the display raylib renders to.
 *
 * Call before InitWindow(). The panel must already be initialised and, for
 * PSRAM-only panels, its framebuffer must be reachable from DMA.
 *
 * @return ESP_OK, or ESP_ERR_INVALID_ARG on a bad config
 */
esp_err_t esp_raylib_set_display(const esp_raylib_display_config_t *config);

#ifdef __cplusplus
}
#endif
