/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_raylib.h"

#define TAG "raylib.idf"

typedef struct {
    esp_raylib_display_config_t cfg;
    bool configured;
    int64_t start_us;    /* GetTime() epoch, set by InitPlatform */
} esp_raylib_state_t;

extern esp_raylib_state_t esp_raylib_state;
