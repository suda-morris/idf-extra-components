/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * Serial OTA over YMODEM on a UHCI (UART DMA) pipe.
 *
 * Why YMODEM and not classic XMODEM: block 0 declares the exact image size,
 * which esp_ota_begin() needs — XMODEM pads the tail with 0x1A and the real
 * length is unknowable on the wire. 1K blocks quarter the ACK round trips.
 * The batch carries a second file, "<app>.sha256", whose content is checked
 * against the flashed image before the boot partition is switched. Send the
 * pair with any YMODEM tool, e.g. `sb xymodem_ota.bin xymodem_ota.sha256`.
 *
 * Data paths (this is the UHCI showcase of the xymodem driver):
 *   RX: UHCI continuous DMA -> ISR callback -> xymodem_on_receive(), the
 *       driver's ISR path (spinlock-guarded copy into its ingest ring). The
 *       whole chain is IRAM-resident and the DMA buffer is internal RAM, so
 *       reception keeps running while esp_ota_write() below has the flash
 *       cache disabled — CONFIG_UHCI_ISR_CACHE_SAFE=y makes that legal.
 *   TX: emit() -> uhci_transmit() + uhci_wait_all_tx_transaction_done(): the
 *       protocol timers keep running, so the bytes must be on the wire
 *       before emit() returns. A full 1K packet takes ~90 ms at 115200.
 *   Engine: the main loop calls xymodem_poll(); esp_ota_write() inside
 *       data_sink may block for a flash erase/write, during which incoming
 *       bytes keep piling up in the ingest ring from the ISR.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/uhci.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "psa/crypto.h"
#include "sdkconfig.h"
#include "xymodem.h"

static const char *TAG = "xymodem_ota";

#define UART_PORT       ((uart_port_t)CONFIG_EXAMPLE_UART_PORT_NUM)
#define UART_TX_GPIO    CONFIG_EXAMPLE_UART_TX_GPIO
#define UART_RX_GPIO    CONFIG_EXAMPLE_UART_RX_GPIO
#define UART_BAUD       CONFIG_EXAMPLE_UART_BAUD_RATE

/* UHCI RX DMA storage: the driver loops it, so the image may exceed it. */
#define UHCI_RX_DMA_BUF     8192
/* emit() staging: one full 1K packet plus headers/CAN bursts. */
#define TX_STAGE_LEN        1100
/* Streaming chunk for hashing the flashed image back. */
#define HASH_PAGE           4096

typedef enum { RX_NONE, RX_OTA, RX_HASH } rx_mode_t;

typedef struct {
    xymodem_handle_t handle;
    uhci_controller_handle_t uhci;
    uint8_t *dma_buf;
    /* OTA state, touched from the callbacks (driver lock held). */
    rx_mode_t mode;
    bool got_image; /* a .bin header was accepted this batch (mode tracks only
                     * where data_sink currently writes: the batch's LAST file
                     * is the digest, so mode != RX_OTA at completion). */
    esp_ota_handle_t ota;
    const esp_partition_t *part;
    size_t written;
    char digest[97];
    size_t digest_len;
} app_ctx_t;

static app_ctx_t s_app;
/* emit() staging buffer: uhci_transmit() needs the bytes to stay valid until
 * the DMA is done, but the protocol's buffer is only valid during the call. */
static uint8_t s_tx_stage[TX_STAGE_LEN];
/* Hashing scratch, static because it must not grow the poll-loop stack. */
static uint8_t s_hash_page[HASH_PAGE];

/* ---- TX: emit() staged through UHCI DMA ---- */
static esp_err_t app_emit(xymodem_handle_t handle, void *ctx, const uint8_t *data, size_t len)
{
    (void)handle;
    (void)ctx;
    if (len > sizeof(s_tx_stage)) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(s_tx_stage, data, len);
    ESP_RETURN_ON_ERROR(uhci_transmit(s_app.uhci, s_tx_stage, len), TAG, "uhci_transmit failed");
    esp_err_t err = uhci_wait_all_tx_transaction_done(s_app.uhci, 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uhci tx did not finish: %s", esp_err_to_name(err));
    }
    return err;
}

/* ---- RX: UHCI ISR feeds the driver's ingest ring ---- */
static bool IRAM_ATTR uhci_rx_isr(uhci_controller_handle_t uhci,
                                  const uhci_rx_event_data_t *edata, void *user_ctx)
{
    app_ctx_t *app = user_ctx;
    (void)uhci;
    (void)xymodem_on_receive(app->handle, edata->data, edata->recv_size);
    return false; /* the ISR path never wakes a task */
}

/* ---- block 0: open the OTA slot or prepare the digest buffer ---- */
static bool app_on_file(xymodem_handle_t handle, void *ctx, const char *name,
                        uint64_t size, bool has_size)
{
    (void)handle;
    app_ctx_t *app = ctx;
    size_t nlen = strlen(name);

    if (nlen >= 4 && strcmp(name + nlen - 4, ".bin") == 0) {
        const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
        if (part == NULL || !has_size || size == 0 || size > part->size) {
            ESP_LOGE(TAG, "refusing \"%s\": no OTA slot or bad size (%" PRIu64 ")",
                     name, size);
            return false;
        }
        esp_ota_handle_t ota;
        esp_err_t err = esp_ota_begin(part, (size_t)size, &ota);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_begin(%s, %" PRIu64 ") failed: %s",
                     part->label, size, esp_err_to_name(err));
            return false;
        }
        app->mode = RX_OTA;
        app->got_image = true;
        app->ota = ota;
        app->part = part;
        app->written = 0;
        ESP_LOGI(TAG, "receiving \"%s\" (%" PRIu64 " bytes) into %s", name, size, part->label);
        return true;
    }
    if (nlen >= 7 && strcmp(name + nlen - 7, ".sha256") == 0) {
        if (size > sizeof(app->digest) - 1) {
            ESP_LOGE(TAG, "refusing \"%s\": digest too long (%" PRIu64 ")", name, size);
            return false;
        }
        app->mode = RX_HASH;
        app->digest_len = 0;
        ESP_LOGI(TAG, "receiving \"%s\" as the digest", name);
        return true;
    }
    ESP_LOGW(TAG, "refusing \"%s\": not a .bin/.sha256 file", name);
    return false;
}

/* ---- payload: stream into flash / collect the digest ---- */
static esp_err_t app_sink(xymodem_handle_t handle, void *ctx, const uint8_t *data, size_t len)
{
    (void)handle;
    app_ctx_t *app = ctx;
    switch (app->mode) {
    case RX_OTA: {
        /* May block on flash erase/write for ~100 ms; the UHCI ISR keeps
         * feeding the ingest ring meanwhile (cache-safe ISR config). */
        esp_err_t err = esp_ota_write(app->ota, data, len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            return err;
        }
        app->written += len;
        return ESP_OK;
    }
    case RX_HASH:
        if (app->digest_len + len > sizeof(app->digest) - 1) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(app->digest + app->digest_len, data, len);
        app->digest_len += len;
        return ESP_OK;
    default:
        return ESP_FAIL; /* payload before any header: not an OTA image */
    }
}

/* ---- lifecycle ---- */
static void app_event(xymodem_handle_t handle, void *ctx, xymodem_event_t event, xymodem_err_t arg)
{
    (void)handle;
    (void)ctx;
    (void)arg;
    switch (event) {
    case XYMODEM_EV_FILE_COMPLETE:
        if (s_app.mode == RX_OTA) {
            ESP_LOGI(TAG, "image written (%u bytes)", (unsigned)s_app.written);
        } else if (s_app.mode == RX_HASH) {
            ESP_LOGI(TAG, "digest received (%u bytes)", (unsigned)s_app.digest_len);
        }
        break;
    case XYMODEM_EV_FAILED:
        ESP_LOGE(TAG, "transfer failed: %d", (int)arg);
        break;
    default:
        break;
    }
}

/* Hash the flashed image back and compare with the received digest. */
static bool image_matches_digest(const app_ctx_t *app)
{
    if (app->digest_len < 64) {
        ESP_LOGW(TAG, "no digest file in the batch; skipping verification");
        return true;
    }
    /* IDF 6 ships mbedtls 4: streaming SHA-2 goes through the PSA API. */
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    psa_status_t st = psa_hash_setup(&op, PSA_ALG_SHA_256);
    for (size_t off = 0; st == PSA_SUCCESS && off < app->written; off += HASH_PAGE) {
        size_t n = (app->written - off < HASH_PAGE) ? (app->written - off) : HASH_PAGE;
        if (esp_partition_read(app->part, off, s_hash_page, n) != ESP_OK) {
            st = PSA_ERROR_COMMUNICATION_FAILURE;
            break;
        }
        st = psa_hash_update(&op, s_hash_page, n);
    }
    uint8_t sha[32];
    size_t sha_len = 0;
    if (st == PSA_SUCCESS) {
        st = psa_hash_finish(&op, sha, sizeof(sha), &sha_len);
    }
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "hashing the flashed image failed: %d", (int)st);
        return false;
    }
    static const char hex_digits[] = "0123456789abcdef";
    char hex[65];
    for (int i = 0; i < 32; i++) {
        hex[2 * i] = hex_digits[sha[i] >> 4];
        hex[2 * i + 1] = hex_digits[sha[i] & 0xF];
    }
    hex[64] = '\0';
    if (strncasecmp(hex, app->digest, 64) != 0) {
        ESP_LOGE(TAG, "digest mismatch: expected %.64s, flashed %.64s", app->digest, hex);
        return false;
    }
    return true;
}

static void reset_for_retry(app_ctx_t *app)
{
    if (app->mode == RX_OTA) {
        esp_ota_abort(app->ota);
        ESP_LOGW(TAG, "%s holds an aborted image; the running slot is untouched",
                 app->part ? app->part->label : "the OTA slot");
    }
    app->mode = RX_NONE;
    app->got_image = false;
    app->written = 0;
    app->digest_len = 0;
}

static void ota_finish(app_ctx_t *app)
{
    if (!app->got_image) {
        ESP_LOGW(TAG, "no image received; waiting for the next upload");
        app->mode = RX_NONE;
        return;
    }
    esp_err_t err = esp_ota_end(app->ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        reset_for_retry(app);
        return;
    }
    if (!image_matches_digest(app)) {
        reset_for_retry(app);
        return;
    }
    ESP_ERROR_CHECK(esp_ota_set_boot_partition(app->part));
    ESP_LOGI(TAG, "image verified, booting into %s...", app->part->label);
    vTaskDelay(pdMS_TO_TICKS(100)); /* let the log drain */
    esp_restart();
}

void app_main(void)
{
    ESP_LOGI(TAG, "running from partition: %s", esp_ota_get_running_partition()->label);

    uart_config_t uart_cfg = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_GPIO, UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    uhci_controller_config_t uhci_cfg = {
        .uart_port = UART_PORT,
        .tx_trans_queue_depth = 4,
        .max_receive_internal_mem = UHCI_RX_DMA_BUF,
        .max_transmit_size = UHCI_RX_DMA_BUF,
        .dma_burst_size = 32,
        .rx_eof_flags.idle_eof = 1, /* deliver chunks as the line turns idle */
    };
    ESP_ERROR_CHECK(uhci_new_controller(&uhci_cfg, &s_app.uhci));

    xymodem_config_t cfg = {
        .core = XYMODEM_CORE_CONFIG_DEFAULT(),
        .emit = app_emit,
        .data_sink = app_sink,
        .on_file = app_on_file,
        .on_event = app_event,
        .user_ctx = &s_app,
    };
    ESP_ERROR_CHECK(xymodem_new(&cfg, &s_app.handle));

    /* Internal RAM: the UHCI RX DMA writes this while the cache may be off. */
    s_app.dma_buf = heap_caps_calloc(1, UHCI_RX_DMA_BUF, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_app.dma_buf == NULL) {
        ESP_LOGE(TAG, "no memory for the UHCI RX DMA buffer");
        return;
    }

    uhci_event_callbacks_t cbs = {
        .on_rx_trans_event = uhci_rx_isr,
    };
    ESP_ERROR_CHECK(uhci_register_event_callbacks(s_app.uhci, &cbs, &s_app));
    ESP_ERROR_CHECK(uhci_start_receive_continuous(s_app.uhci, s_app.dma_buf, UHCI_RX_DMA_BUF));

    ESP_ERROR_CHECK(psa_crypto_init() == PSA_SUCCESS ? ESP_OK : ESP_FAIL);
    ESP_LOGI(TAG, "waiting for the OTA image (YMODEM: <app>.bin [+ <app>.sha256])");
    ESP_ERROR_CHECK(xymodem_start_recv(s_app.handle));

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10)); /* the read timeout is the poll period */
        ESP_ERROR_CHECK(xymodem_poll(s_app.handle));
        xymodem_status_t st = xymodem_status(s_app.handle);
        if (st == XYMODEM_ST_COMPLETE) {
            ota_finish(&s_app); /* returns only on a failed/rejected upload */
            ESP_ERROR_CHECK(xymodem_start_recv(s_app.handle));
        } else if (st == XYMODEM_ST_FAILED) {
            reset_for_retry(&s_app);
            ESP_ERROR_CHECK(xymodem_start_recv(s_app.handle));
        }
    }
}
