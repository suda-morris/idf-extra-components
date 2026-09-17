/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Screenshot helpers for the esp_lcd_host panel driver.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "sys/param.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "esp_lcd_host_panel.h"
#include "png.h"

static const char *TAG = "lcd_host.shot";

#define HOST_BASE64_CHUNK_IN     3072
#define HOST_BASE64_CHUNK_OUT    (((HOST_BASE64_CHUNK_IN + 2) / 3) * 4)
#define HOST_BASE64_BUFFER_SIZE  (HOST_BASE64_CHUNK_OUT + 1)

/* Print the fourcc the same way esp_lcd_screenshot does: little endian packed,
 * so the first character is the least significant byte. */
static void host_panel_fourcc_str(esp_color_fourcc_t fourcc, char out[5])
{
    out[0] = (char)(fourcc & 0xff);
    out[1] = (char)((fourcc >> 8) & 0xff);
    out[2] = (char)((fourcc >> 16) & 0xff);
    out[3] = (char)((fourcc >> 24) & 0xff);
    out[4] = '\0';
}

static uint8_t rgb565_to_rgb888_r(uint16_t c)
{
    uint8_t r = (c >> 11) & 0x1f;
    return (uint8_t)((r << 3) | (r >> 2));
}

static uint8_t rgb565_to_rgb888_g(uint16_t c)
{
    uint8_t g = (c >> 5) & 0x3f;
    return (uint8_t)((g << 2) | (g >> 4));
}

static uint8_t rgb565_to_rgb888_b(uint16_t c)
{
    uint8_t b = c & 0x1f;
    return (uint8_t)((b << 3) | (b >> 2));
}

static bool host_screenshot_bgra_alpha_all_zero(const uint8_t *framebuffer, size_t pixel_count)
{
    for (size_t i = 0; i < pixel_count; i++) {
        if (framebuffer[i * 4 + 3] != 0) {
            return false;
        }
    }
    return true;
}

/* Fill one PNG scanline (RGB or RGBA, 8 bits per channel) from framebuffer row y. */
static void host_screenshot_fill_png_row(const uint8_t *framebuffer, esp_color_fourcc_t color_format, int width, int y,
                                         png_bytep row, unsigned char channels)
{
    switch (color_format) {
    case ESP_COLOR_FOURCC_RGB16:
    case ESP_COLOR_FOURCC_RGB16_BE: {
        bool big_endian = (color_format == ESP_COLOR_FOURCC_RGB16_BE);
        const uint8_t *src = framebuffer + (size_t)y * width * 2;
        for (int x = 0; x < width; x++) {
            uint16_t c = big_endian
                         ? ((uint16_t)src[x * 2] << 8) | src[x * 2 + 1]
                         : src[x * 2] | ((uint16_t)src[x * 2 + 1] << 8);
            row[x * 3 + 0] = rgb565_to_rgb888_r(c);
            row[x * 3 + 1] = rgb565_to_rgb888_g(c);
            row[x * 3 + 2] = rgb565_to_rgb888_b(c);
        }
        break;
    }
    case ESP_COLOR_FOURCC_RGB24:
        memcpy(row, framebuffer + (size_t)y * width * 3, (size_t)width * 3);
        break;
    case ESP_COLOR_FOURCC_BGR24: {
        const uint8_t *src = framebuffer + (size_t)y * width * 3;
        for (int x = 0; x < width; x++) {
            row[x * 3 + 0] = src[x * 3 + 2];
            row[x * 3 + 1] = src[x * 3 + 1];
            row[x * 3 + 2] = src[x * 3 + 0];
        }
        break;
    }
    case ESP_COLOR_FOURCC_BGRA32: {
        /* Bytes in memory are B, G, R, A. */
        const uint8_t *src = framebuffer + (size_t)y * width * 4;
        for (int x = 0; x < width; x++) {
            row[x * channels + 0] = src[x * 4 + 2];
            row[x * channels + 1] = src[x * 4 + 1];
            row[x * channels + 2] = src[x * 4 + 0];
            if (channels == 4) {
                row[x * 4 + 3] = src[x * 4 + 3];
            }
        }
        break;
    }
    default:
        memset(row, 0, (size_t)width * channels);
        break;
    }
}

esp_err_t esp_lcd_host_screenshot_save_png(esp_lcd_panel_handle_t panel, const char *filepath)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(filepath, ESP_ERR_INVALID_ARG, TAG, "filepath is NULL");

    esp_lcd_host_panel_info_t info = {0};
    ESP_RETURN_ON_ERROR(esp_lcd_host_panel_get_info(panel, &info), TAG, "invalid panel handle");

    /* The alpha channel of 32bpp formats is often unused. Treat an all-zero
     * alpha as opaque so the resulting PNG is useful instead of fully
     * transparent. */
    bool xrgb_as_opaque = false;
    unsigned char channels = 3;
    if (info.bytes_per_pixel == 4) {
        xrgb_as_opaque = host_screenshot_bgra_alpha_all_zero(info.framebuffer, (size_t)info.width * info.height);
        channels = xrgb_as_opaque ? 3 : 4;
    }

    FILE *f = fopen(filepath, "wb");
    ESP_GOTO_ON_FALSE(f, ESP_FAIL, err, TAG, "failed to open '%s'", filepath);

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    ESP_GOTO_ON_FALSE(png_ptr, ESP_ERR_NO_MEM, err_close_file, TAG, "png_create_write_struct failed");

    png_infop info_ptr = png_create_info_struct(png_ptr);
    png_bytep row = malloc((size_t)info.width * channels);
    ESP_GOTO_ON_FALSE(info_ptr && row, ESP_ERR_NO_MEM, err_destroy_png, TAG, "no memory for PNG encoder");

    /* libpng reports write errors through longjmp rather than return values;
     * release every resource here before propagating the failure. */
    if (setjmp(png_jmpbuf(png_ptr))) {
        ret = ESP_FAIL;
        ESP_LOGE(TAG, "failed to write PNG file '%s'", filepath);
        goto err_destroy_png;
    }

    png_init_io(png_ptr, f);
    png_set_IHDR(png_ptr, info_ptr, (png_uint_32)info.width, (png_uint_32)info.height, 8,
                 channels == 4 ? PNG_COLOR_TYPE_RGB_ALPHA : PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_set_sRGB(png_ptr, info_ptr, PNG_sRGB_INTENT_PERCEPTUAL);
    png_write_info(png_ptr, info_ptr);

    for (int y = 0; y < info.height; y++) {
        host_screenshot_fill_png_row(info.framebuffer, info.color_format, info.width, y, row, channels);
        png_write_row(png_ptr, row);
    }
    png_write_end(png_ptr, NULL);

    long written = ftell(f);
    png_destroy_write_struct(&png_ptr, &info_ptr);
    free(row);
    fclose(f);

    ESP_LOGI(TAG, "Saved %dx%d PNG image (%ld bytes) to '%s'", info.width, info.height, written, filepath);
    return ESP_OK;

err_destroy_png:
    free(row);
    png_destroy_write_struct(&png_ptr, info_ptr ? &info_ptr : NULL);
err_close_file:
    fclose(f);
    /* fopen(..., "wb") already truncated the path; do not leave a partial PNG. */
    if (remove(filepath) != 0) {
        ESP_LOGW(TAG, "failed to remove incomplete PNG '%s'", filepath);
    }
err:
    return ret;
}

esp_err_t esp_lcd_host_screenshot_dump_base64(esp_lcd_panel_handle_t panel, FILE *stream)
{
    esp_lcd_host_panel_info_t info = {0};
    ESP_RETURN_ON_ERROR(esp_lcd_host_panel_get_info(panel, &info), TAG, "invalid panel handle");
    if (!stream) {
        stream = stdout;
    }

    char fourcc[5];
    host_panel_fourcc_str(info.color_format, fourcc);

    unsigned char *encoded = malloc(HOST_BASE64_BUFFER_SIZE);
    ESP_RETURN_ON_FALSE(encoded, ESP_ERR_NO_MEM, TAG, "no mem for base64 buffer");

    fprintf(stream, "FRAMEBUFFER_BEGIN %d %d %s\n", info.width, info.height, fourcc);
    size_t remaining = info.framebuffer_size;
    const unsigned char *src = info.framebuffer;
    while (remaining > 0) {
        size_t chunk = MIN(remaining, (size_t)HOST_BASE64_CHUNK_IN);
        size_t encoded_len = 0;
        mbedtls_base64_encode(encoded, HOST_BASE64_BUFFER_SIZE, &encoded_len, src, chunk);
        encoded[encoded_len] = '\0';
        fprintf(stream, "FB_BASE64 %s\n", encoded);
        /* Let the idle task and the task watchdog run while the framebuffer is
         * streamed out through a slow console. */
        vTaskDelay(1);
        src += chunk;
        remaining -= chunk;
    }
    fprintf(stream, "FRAMEBUFFER_END\n");
    fflush(stream);

    free(encoded);
    ESP_LOGI(TAG, "Dumped %dx%d %s framebuffer as base64", info.width, info.height, fourcc);
    return ESP_OK;
}
