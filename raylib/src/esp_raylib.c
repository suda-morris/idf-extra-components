/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_raylib_private.h"
#include "esp_check.h"

esp_raylib_state_t esp_raylib_state;

esp_err_t esp_raylib_set_display(const esp_raylib_display_config_t *config)
{
    ESP_RETURN_ON_FALSE(config && config->handle && config->width && config->height && config->flush_cb, ESP_ERR_INVALID_ARG, TAG, "Invalid display config");

    esp_raylib_state.cfg = *config;
    esp_raylib_state.configured = true;

    return ESP_OK;
}
