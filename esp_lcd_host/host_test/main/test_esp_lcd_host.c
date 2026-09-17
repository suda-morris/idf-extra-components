/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * Host tests for the esp_lcd_host panel driver. The panel is exercised through
 * the public esp_lcd panel API only, plus the screenshot helpers of the
 * component.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "unity.h"
#include "esp_lcd_host.h"

#define TEST_WIDTH      64
#define TEST_HEIGHT     32
#define TEST_PNG_PATH   "esp_lcd_host_test.png"
#define TEST_BAD_PATH   "/nonexistent-dir/esp_lcd_host_test.png"

static esp_lcd_panel_handle_t create_panel(esp_lcd_host_config_t *config)
{
    esp_lcd_panel_handle_t panel = NULL;
    TEST_ESP_OK(esp_lcd_new_panel_host_sdl(config, &panel));
    TEST_ASSERT_NOT_NULL(panel);
    TEST_ESP_OK(esp_lcd_panel_reset(panel));
    TEST_ESP_OK(esp_lcd_panel_init(panel));
    return panel;
}

static size_t file_size(const char *path)
{
    struct stat st = {0};
    if (stat(path, &st) != 0) {
        return 0;
    }
    return (size_t)st.st_size;
}

TEST_CASE("esp_lcd_host: invalid arguments are rejected", "[esp_lcd_host]")
{
    esp_lcd_host_config_t config = {
        .width = TEST_WIDTH,
        .height = TEST_HEIGHT,
        .color_format = ESP_COLOR_FOURCC_RGB24,
    };
    esp_lcd_panel_handle_t panel = NULL;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esp_lcd_new_panel_host_sdl(NULL, &panel));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esp_lcd_new_panel_host_sdl(&config, NULL));

    config.width = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esp_lcd_new_panel_host_sdl(&config, &panel));

    config.width = TEST_WIDTH;
    config.color_format = (esp_color_fourcc_t)0x30303030; /* "0000" is not a format */
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, esp_lcd_new_panel_host_sdl(&config, &panel));

    /* The screenshot helpers and the target query only accept their own handle. */
    esp_lcd_host_target_t target = ESP_LCD_HOST_TARGET_ESP32;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esp_lcd_host_get_target(NULL, &target));
    /* A panel object that this driver does not own is rejected as well. */
    esp_lcd_panel_t foreign_panel = {0};
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esp_lcd_host_get_target(&foreign_panel, &target));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esp_lcd_host_screenshot_save_png(NULL, TEST_PNG_PATH));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esp_lcd_host_return_panel(NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esp_lcd_host_return_buffers(NULL));
}

TEST_CASE("esp_lcd_host: panel reports the POSIX target", "[esp_lcd_host]")
{
    esp_lcd_host_config_t config = {
        .width = TEST_WIDTH,
        .height = TEST_HEIGHT,
        .color_format = ESP_COLOR_FOURCC_RGB24,
    };
    esp_lcd_panel_handle_t panel = create_panel(&config);

    esp_lcd_host_target_t target = ESP_LCD_HOST_TARGET_ESP32;
    TEST_ESP_OK(esp_lcd_host_get_target(panel, &target));
    TEST_ASSERT_EQUAL(ESP_LCD_HOST_TARGET_POSIX, target);

    TEST_ESP_OK(esp_lcd_host_return_buffers(panel));
    TEST_ESP_OK(esp_lcd_panel_del(panel));
}

TEST_CASE("esp_lcd_host: frames are stored and exported as PNG", "[esp_lcd_host]")
{
    esp_lcd_host_config_t config = {
        .width = TEST_WIDTH,
        .height = TEST_HEIGHT,
        .color_format = ESP_COLOR_FOURCC_BGR24,
        .create_window = false, /* no display needed in CI */
    };
    esp_lcd_panel_handle_t panel = create_panel(&config);

    /* Paint a colored gradient so the PNG is not a constant image. */
    uint8_t *frame = malloc((size_t)TEST_WIDTH * TEST_HEIGHT * 3);
    TEST_ASSERT_NOT_NULL(frame);
    for (int y = 0; y < TEST_HEIGHT; y++) {
        for (int x = 0; x < TEST_WIDTH; x++) {
            size_t offset = ((size_t)y * TEST_WIDTH + x) * 3;
            frame[offset + 0] = (uint8_t)(x * 4);           /* B */
            frame[offset + 1] = (uint8_t)(y * 8);           /* G */
            frame[offset + 2] = (uint8_t)(255 - x * 4);     /* R */
        }
    }

    TEST_ESP_OK(esp_lcd_panel_draw_bitmap(panel, 0, 0, TEST_WIDTH, TEST_HEIGHT, frame));

    TEST_ESP_OK(esp_lcd_host_screenshot_save_png(panel, TEST_PNG_PATH));
    TEST_ASSERT_GREATER_THAN(100, file_size(TEST_PNG_PATH));

    /* A frame drawn partially outside the panel is clipped, not rejected. */
    TEST_ESP_OK(esp_lcd_panel_draw_bitmap(panel, -5, -5, 5, 5, frame));
    TEST_ESP_OK(esp_lcd_host_screenshot_save_png(panel, TEST_PNG_PATH));

    /* An invalid file path fails, and no file is left behind. */
    TEST_ASSERT_EQUAL(ESP_FAIL, esp_lcd_host_screenshot_save_png(panel, TEST_BAD_PATH));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, esp_lcd_host_screenshot_save_png(panel, NULL));

    free(frame);
    TEST_ESP_OK(esp_lcd_panel_del(panel));

    struct stat st = {0};
    TEST_ASSERT_EQUAL(0, remove(TEST_PNG_PATH));
    TEST_ASSERT_NOT_EQUAL(0, stat(TEST_PNG_PATH, &st));
    /* No partial file was left behind. */
    TEST_ASSERT_NOT_EQUAL(0, stat(TEST_BAD_PATH, &st));
}

TEST_CASE("esp_lcd_host: framebuffer is dumped as base64", "[esp_lcd_host]")
{
    esp_lcd_host_config_t config = {
        .width = TEST_WIDTH,
        .height = TEST_HEIGHT,
        .color_format = ESP_COLOR_FOURCC_RGB16,
        .create_window = false,
    };
    esp_lcd_panel_handle_t panel = create_panel(&config);

    /* The dump is meant for a console; only check that both the stream and the
     * default stdout path work. */
    TEST_ESP_OK(esp_lcd_host_screenshot_dump_base64(panel, stdout));
    TEST_ESP_OK(esp_lcd_host_screenshot_dump_base64(panel, NULL));
    TEST_ESP_OK(esp_lcd_panel_del(panel));
}

TEST_CASE("esp_lcd_host: several panels with different sizes can coexist", "[esp_lcd_host]")
{
    esp_lcd_host_config_t small = {
        .width = TEST_WIDTH,
        .height = TEST_HEIGHT,
        .color_format = ESP_COLOR_FOURCC_BGR24,
    };
    esp_lcd_host_config_t large = {
        .width = TEST_WIDTH * 2,
        .height = TEST_HEIGHT * 2,
        .color_format = ESP_COLOR_FOURCC_RGB16,
    };

    esp_lcd_panel_handle_t small_panel = create_panel(&small);
    esp_lcd_panel_handle_t large_panel = create_panel(&large);

    /* The configuration is stored per panel: an export of the small panel must
     * keep using the geometry it was created with. A 64x32 PNG is around 100
     * bytes, a mixed up 128x64 export would be noticeably larger. */
    TEST_ESP_OK(esp_lcd_host_screenshot_save_png(small_panel, TEST_PNG_PATH));
    TEST_ASSERT_GREATER_THAN(60, file_size(TEST_PNG_PATH));
    TEST_ASSERT_LESS_THAN(200, file_size(TEST_PNG_PATH));

    TEST_ESP_OK(esp_lcd_panel_del(small_panel));
    TEST_ESP_OK(esp_lcd_panel_del(large_panel));
    remove(TEST_PNG_PATH);
}
