/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Drives a fireworks show (see fireworks.c) on a bare ST7789 SPI panel. This
 * file is only the display wiring and the raylib frame loop; the scene lives
 * in fireworks.c.
 */

#include "fireworks.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "raylib.h"
#include "esp_raylib.h"

static const char *TAG = "raylib_fireworks";

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;

/* ESP32-S3-BOX style wiring, adjust to your board. */
#define LCD_HOST        SPI3_HOST
#define LCD_MOSI        GPIO_NUM_6
#define LCD_SCLK        GPIO_NUM_7
#define LCD_CS          GPIO_NUM_5
#define LCD_DC          GPIO_NUM_4
#define LCD_RST         GPIO_NUM_40
#define LCD_BACKLIGHT   GPIO_NUM_41

#define LCD_H_RES       320
#define LCD_V_RES       240
#define LCD_PIXEL_CLOCK (40 * 1000 * 1000)  /* 80 MHz glitches on many ST7789 modules */
#define LCD_BAND_ROWS   48

static SemaphoreHandle_t s_xfer_done;

/* draw_bitmap() queues the DMA and returns early, so the flush callback waits
 * here for the pixels to leave the staging buffer (see esp_raylib.h). */
static bool IRAM_ATTR color_trans_done_cb(esp_lcd_panel_io_handle_t io,
                                          esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)io;
    (void)edata;
    (void)user_ctx;
    BaseType_t high_task_wakeup = pdFALSE;
    xSemaphoreGiveFromISR(s_xfer_done, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

static void panel_flush(esp_raylib_display_handle_t handle, const uint16_t *pixels,
                        uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap((esp_lcd_panel_handle_t)handle,
                                              x, y, x + w, y + h, pixels));
    xSemaphoreTake(s_xfer_done, portMAX_DELAY);
}

static esp_err_t display_init(void)
{
    const spi_bus_config_t bus_config = {
        .mosi_io_num = LCD_MOSI,
        .sclk_io_num = LCD_SCLK,
        /* unused pins must be -1: a zeroed field is a valid GPIO */
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_H_RES * LCD_BAND_ROWS * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO),
                        TAG, "SPI bus init failed");

    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_CS,
        .dc_gpio_num = LCD_DC,
        .pclk_hz = LCD_PIXEL_CLOCK,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = {
            // Use the PSRAM color buffer directly for DMA instead of a temporary copy.
            .psram_dma_direct = true,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                                 &io_config, &s_io),
                        TAG, "panel IO init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(s_io,
    &(esp_lcd_panel_io_callbacks_t) {
        .on_color_trans_done = color_trans_done_cb
    },
    NULL), TAG, "callback registration failed");

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,   /* has to match CONFIG_RAYLIB_FRAMEBUFFER_FORMAT */
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_io, &panel_config, &s_panel),
                        TAG, "panel init failed");

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    const gpio_config_t backlight = {
        .pin_bit_mask = (1ULL << LCD_BACKLIGHT),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&backlight));
    gpio_set_level(LCD_BACKLIGHT, 0);

    return ESP_OK;
}

static void raylib_task(void *arg)
{
    (void)arg;

    const esp_raylib_display_config_t display = {
        .handle = s_panel,
        .width = LCD_H_RES,
        .height = LCD_V_RES,
        .swap_bytes = true,    /* ST7789 over SPI takes RGB565 MSB first */
        .invert = true,        /* panel origin at the bottom */
        .chunk_rows = LCD_BAND_ROWS,
        .flush_cb = panel_flush,
    };

    if (esp_raylib_set_display(&display) != ESP_OK) {
        ESP_LOGE(TAG, "esp_raylib_set_display() failed");
        vTaskDelete(NULL);
        return;
    }

    InitWindow(LCD_H_RES, LCD_V_RES, "raylib fireworks");
    /* Pacing happens in the platform layer's SwapScreenBuffer(): it blocks
     * with vTaskDelay() for the remainder of the target frame, which both
     * spaces frames and keeps the idle task this core's watchdog watches
     * alive -- a software-rendered loop that never blocks trips the task
     * watchdog within seconds. The SPI wire (about 30 ms per full frame at
     * 40 MHz) is the real limiter here; the target only matters on faster
     * panels. */
    SetTargetFPS(60);
    SetRandomSeed((unsigned)esp_timer_get_time());
    fireworks_init(LCD_H_RES, LCD_V_RES);

    while (!WindowShouldClose()) {
        /* raw frame time: fireworks_update paces itself with a fixed-step
         * accumulator, so the show stays in sync with wall time */
        fireworks_update(GetFrameTime());

        BeginDrawing();
        fireworks_draw((float)GetTime());
        EndDrawing();
    }

    CloseWindow();
    vTaskDelete(NULL);
}

void app_main(void)
{
    s_xfer_done = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(display_init());

    /* The rasteriser needs deep stack on the calling task; 64 KiB is a
     * comfortable start. */
    if (xTaskCreatePinnedToCore(raylib_task, "raylib", 64 * 1024, NULL, 5, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "failed to start raylib task");
    }
}
