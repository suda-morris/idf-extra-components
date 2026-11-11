/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Panel-private state, shared with the screenshot helpers only.
 *
 * The panel object itself is private to esp_lcd_sim.c; the PNG encoder lives
 * in its own translation unit and reads the panel through the two accessors
 * below. It has no business with the preview backend, so this header says
 * nothing about SDL.
 */

/**
 * @brief Snapshot of the state of an esp_lcd_sim panel
 *
 * Used by the screenshot helpers, which live in a separate translation unit and
 * therefore cannot read the private panel object directly.
 */
typedef struct {
    int width;                      /*!< Panel width in pixels */
    int height;                     /*!< Panel height in pixels */
    size_t bytes_per_pixel;         /*!< Bytes per pixel of `color_format` */
    esp_color_fourcc_t color_format;/*!< Framebuffer color format */
    size_t framebuffer_size;        /*!< Size of the framebuffer in bytes */
} esp_lcd_sim_panel_info_t;

/**
 * @brief Read the immutable geometry of an esp_lcd_sim panel
 *
 * The returned fields are fixed at creation time, so they can be read without
 * the panel lock. The pixel data is deliberately not exposed here: read it
 * with esp_lcd_sim_panel_snapshot_framebuffer(), which copies it under the
 * lock so a concurrent draw cannot tear the frame.
 *
 * @param[in]  panel Panel handle created by esp_lcd_new_panel_sim_sdl()
 * @param[out] info  Returned panel state
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for an unknown handle
 */
esp_err_t esp_lcd_sim_panel_get_info(esp_lcd_panel_handle_t panel, esp_lcd_sim_panel_info_t *info);

/**
 * @brief Copy the panel framebuffer into a caller buffer under the panel lock
 *
 * Takes a consistent snapshot of the whole framebuffer, safe to call from a
 * thread other than the GUI thread (for example the console command thread
 * exporting a frame). The caller allocates `dst` with at least
 * esp_lcd_sim_panel_info_t::framebuffer_size bytes; the copy is in the panel's
 * native color format. The lock is released before this returns, so the caller
 * can encode the snapshot without blocking the GUI.
 *
 * @param[in]  panel    Panel handle created by esp_lcd_new_panel_sim_sdl()
 * @param[out] dst      Destination buffer, at least framebuffer_size bytes
 * @param[in]  dst_size Size of dst in bytes
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for an unknown handle, a NULL
 *         dst or a dst smaller than the framebuffer
 */
esp_err_t esp_lcd_sim_panel_snapshot_framebuffer(esp_lcd_panel_handle_t panel, uint8_t *dst, size_t dst_size);

#ifdef __cplusplus
} // extern "C"
#endif
