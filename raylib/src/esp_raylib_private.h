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

/* Fixed internal default for the frame-present task. */
#define ESP_RAYLIB_PRESENT_STACK_SIZE 4096

/* State behind an esp_raylib_display_handle_t */
typedef struct esp_raylib_display_t {
    esp_raylib_flush_cb_t flush_cb;
    void *user_data;
    uint16_t chunk_rows;
    int64_t start_us;
    bool swap_bytes;
    bool added;  /* enforces the single display, guards InitPlatform() */
} esp_raylib_display_t;

/* The single display instance. */
extern esp_raylib_display_t g_esp_raylib_display;

/**
 * Routing every access through esp_raylib_current() leaves the
 * one seam a future multi-display implementation has to change to select
 * among several displays.
 */
static inline esp_raylib_display_t *esp_raylib_current(void)
{
    return &g_esp_raylib_display;
}
