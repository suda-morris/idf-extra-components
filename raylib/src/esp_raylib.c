/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_raylib_private.h"
#include "esp_log.h"

esp_raylib_display_t g_esp_raylib_display;

esp_raylib_display_handle_t esp_raylib_add_display(const esp_raylib_display_config_t *config)
{
    if ((config == NULL) || (config->flush_cb == NULL)) {
        ESP_LOGE(TAG, "Invalid display config: flush_cb is required");
        return NULL;
    }
    if (g_esp_raylib_display.added) {
        ESP_LOGE(TAG, "only one display is currently supported");
        return NULL;
    }

    g_esp_raylib_display.flush_cb = config->flush_cb;
    g_esp_raylib_display.user_data = config->user_data;
    g_esp_raylib_display.chunk_rows = config->chunk_rows;
    g_esp_raylib_display.swap_bytes = config->swap_bytes;
    g_esp_raylib_display.start_us = 0;
    g_esp_raylib_display.added = true;
    return &g_esp_raylib_display;
}
