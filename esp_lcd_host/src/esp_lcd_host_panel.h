/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include "sdkconfig.h"
#include "esp_lcd_host.h"
#include "esp_lcd_types.h"
#include "esp_lcd_panel_interface.h"

/**
 * Backends driving an esp_lcd_host panel.
 *
 * The panel object lives in esp_lcd_host.c and issues every backend call from
 * there, so the panel can also use backends that are only available for a
 * subset of the ESP_LCD_HOST_TARGET_* values.
 */

/**
 * @brief Create the SDL surfaces backing one panel
 *
 * The backend owns the surface holding the framebuffer of the panel, so the
 * panel itself never talks to SDL directly and stays independent of the SDL
 * version in use.
 *
 * @param[in]  config        Panel configuration, copied by the backend
 * @param[in]  framebuffer   Panel framebuffer, used as preview window surface
 * @param[in]  framebuffer_size Size of the framebuffer in bytes
 * @param[out] ret_ctx       Returned backend context, NULL if the backend has no state
 * @return ESP_OK on success
 */
esp_err_t esp_lcd_host_sdl_backend_create(const esp_lcd_host_config_t *config, uint8_t *framebuffer,
                                          size_t framebuffer_size, void **ret_ctx);

/**
 * @brief Release one SDL backend, called from esp_lcd_panel_del()
 */
void esp_lcd_host_sdl_backend_delete(void *ctx);

/**
 * @brief Push the latest panel content to the preview
 *
 * Marks the whole panel dirty: the next esp_lcd_host_pump_events() uploads the
 * complete framebuffer into the preview texture and presents it.
 */
esp_err_t esp_lcd_host_sdl_backend_update(void *ctx, const uint8_t *framebuffer, size_t framebuffer_size);

/**
 * @brief Push a panel region to the preview
 *
 * Marks the region dirty and unions it with the regions of the not yet
 * presented flushes, so a GUI that flushes in partial areas uploads only what
 * changed on the next pump call.
 *
 * @param[in] x      Left edge of the region in panel pixels
 * @param[in] y      Top edge of the region in panel pixels
 * @param[in] width  Width of the region in pixels
 * @param[in] height Height of the region in pixels
 */
esp_err_t esp_lcd_host_sdl_backend_update_rect(void *ctx, const uint8_t *framebuffer, size_t framebuffer_size,
                                               int x, int y, int width, int height);

/**
 * @brief Pump the SDL event queue, called from esp_lcd_host_pump_events()
 */
esp_err_t esp_lcd_host_sdl_backend_pump(void *ctx);

/**
 * @brief Convert a fourcc to the SDL pixel format storing the same bytes
 *
 * @param[in]  fourcc        Framebuffer color format
 * @param[out] ret_format    Returned SDL pixel format
 * @param[out] ret_bytes     Returned bytes per pixel
 * @return ESP_OK on success, ESP_ERR_NOT_SUPPORTED for an unknown fourcc
 */
esp_err_t esp_lcd_host_sdl_pixel_format(esp_color_fourcc_t fourcc, uint32_t *ret_format, size_t *ret_bytes);

/**
 * @brief Snapshot of the state of an esp_lcd_host panel
 *
 * Used by the screenshot helpers, which live in a separate translation unit and
 * therefore cannot read the private panel object directly.
 */
typedef struct {
    int width;                      /*!< Panel width in pixels */
    int height;                     /*!< Panel height in pixels */
    size_t bytes_per_pixel;         /*!< Bytes per pixel of `color_format` */
    esp_color_fourcc_t color_format;/*!< Framebuffer color format */
    uint8_t *framebuffer;           /*!< Panel framebuffer, owned by the panel */
    size_t framebuffer_size;        /*!< Size of the framebuffer in bytes */
} esp_lcd_host_panel_info_t;

/**
 * @brief Read the state of an esp_lcd_host panel
 *
 * @param[in]  panel Panel handle created by esp_lcd_new_panel_host_sdl()
 * @param[out] info  Returned panel state
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for an unknown handle
 */
esp_err_t esp_lcd_host_panel_get_info(esp_lcd_panel_handle_t panel, esp_lcd_host_panel_info_t *info);
