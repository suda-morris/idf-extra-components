/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"

#include "raylib.h"
#include "esp_raylib.h"
#include "fireworks.h"

/* All panel and sync state lives here. One instance is created in app_main()
 * and handed to the ISR via user_ctx and to flush_cb via the component's
 * user_data */
typedef struct {
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
    SemaphoreHandle_t sem;
} flush_ctx_t;

#define LCD_HOST        SPI3_HOST
#define LCD_MOSI        GPIO_NUM_6
#define LCD_SCLK        GPIO_NUM_7
#define LCD_CS          GPIO_NUM_5
#define LCD_DC          GPIO_NUM_4
#define LCD_RST         GPIO_NUM_40
#define LCD_BACKLIGHT   GPIO_NUM_41

#define LCD_H_RES       320
#define LCD_V_RES       240
#define LCD_PIXEL_CLOCK (40 * 1000 * 1000)
#define LCD_BAND_ROWS   48

/* draw_bitmap() queues the DMA and returns early; the flush callback must not
 * return until the pixels have left the staging buffer */
static bool color_trans_done_cb(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)io;
    (void)edata;
    flush_ctx_t *ctx = user_ctx;
    BaseType_t high_task_wakeup = pdFALSE;
    xSemaphoreGiveFromISR(ctx->sem, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

static void panel_flush(esp_raylib_display_handle_t display, const void *pixels,
                        uint16_t x, uint16_t y, uint16_t w, uint16_t h, void *user_data)
{
    (void)display;
    flush_ctx_t *ctx = user_data;
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(ctx->panel, x, y, x + w, y + h, pixels));
    xSemaphoreTake(ctx->sem, portMAX_DELAY);
}

static void display_init(flush_ctx_t *ctx)
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
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));

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
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &ctx->io));
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(ctx->io,
    &(esp_lcd_panel_io_callbacks_t) {
        .on_color_trans_done = color_trans_done_cb
    },
    ctx));

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,   /* match CONFIG_RAYLIB_FRAMEBUFFER_FORMAT_RGB565 */
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(ctx->io, &panel_config, &ctx->panel));

    esp_lcd_panel_reset(ctx->panel);
    esp_lcd_panel_init(ctx->panel);
    esp_lcd_panel_swap_xy(ctx->panel, true);
    esp_lcd_panel_disp_on_off(ctx->panel, true);

    const gpio_config_t backlight = {
        .pin_bit_mask = (1ULL << LCD_BACKLIGHT),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&backlight));
    ESP_ERROR_CHECK(gpio_set_level(LCD_BACKLIGHT, 0));
}

static void raylib_task(void *arg)
{
    (void)arg;

    InitWindow(LCD_H_RES, LCD_V_RES, NULL);
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
    /* Lives for the whole program; its address is the only thing shared
     * between the ISR and flush_cb. */
    flush_ctx_t *ctx = calloc(1, sizeof(*ctx));
    ESP_ERROR_CHECK(ctx != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    ctx->sem = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(ctx->sem != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    display_init(ctx);

    /* Register how frames reach the panel before InitWindow() runs in the raylib task.*/
    const esp_raylib_display_config_t display_cfg = {
        .chunk_rows = LCD_BAND_ROWS,
        .swap_bytes = true,    /* ST7789 over SPI takes RGB565 MSB first */
        .flush_cb = panel_flush,
        .user_data = ctx,
    };
    ESP_ERROR_CHECK(esp_raylib_add_display(&display_cfg) != NULL ? ESP_OK : ESP_FAIL);

    /* The rasteriser needs deep stack on the calling task; 64 KiB is a comfortable start. */
    ESP_ERROR_CHECK(xTaskCreate(raylib_task, "raylib", 64 * 1024,
                                NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_FAIL);
}
