/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * Event-driven XMODEM echo. One loop in app_main: uart_read_bytes,
 * xymodem_on_receive, xymodem_poll, xymodem_status. The library itself
 * never blocks on the wire (issue #73).
 *
 * The application state is heap-allocated once in app_main() and handed to
 * the driver through xymodem_config_t::user_ctx. It is intentionally never
 * freed: the echo server runs for the lifetime of the device.
 *
 * The receiving side has no "protocol" setting: it accepts a classic
 * header-less sender and a block-0 (file name/size) sender alike. When a
 * header arrived we echo under the same name and finish the YMODEM batch with
 * a second, empty xymodem_start_send() -- several start_send calls are all a
 * batch is. The same handle and the same callbacks serve every session.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_check.h>
#include <sdkconfig.h>
#include "xymodem.h"

static const char *TAG = "xymodem_event";

#define UART_PORT       ((uart_port_t)CONFIG_EXAMPLE_UART_PORT_NUM)
#define UART_RX_BUF     2048
#define UART_TX_BUF     2048
#define STORAGE_CAP     8192
#define POLL_PERIOD_MS  5

/* Application state shared with the callbacks. */
typedef struct {
    uint8_t  storage[STORAGE_CAP];
    size_t   stored;      /* bytes received and delivered */
    size_t   read_pos;    /* next byte to transmit on echo-back */
    xymodem_handle_t handle;
    char     name[64];    /* block-0 file name, when the peer sent a header */
    uint64_t file_size;   /* declared size; 0 when unknown */
    bool     has_header;  /* this upload carried a block 0 */
} app_ctx_t;

/* ---- physical link: emit() pushes protocol bytes onto UART TX ---- */
static esp_err_t app_emit(xymodem_handle_t handle, void *ctx, const uint8_t *data, size_t len)
{
    (void)handle;
    (void)ctx;
    int n = uart_write_bytes(UART_PORT, (const char *)data, len);
    return (n == (int)len) ? ESP_OK : ESP_FAIL;
}

/* ---- data sink: data_sink() stores each accepted block ---- */
static esp_err_t app_store(xymodem_handle_t handle, void *ctx, const uint8_t *data, size_t len)
{
    (void)handle;
    app_ctx_t *app = ctx;
    if (app->stored + len > sizeof(app->storage)) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(app->storage + app->stored, data, len);
    app->stored += len;
    return ESP_OK;
}

/* ---- block 0: remember what the peer announced, so the echo can send it
 * back under the same name and size. A classic sender never calls this. ---- */
static bool app_on_file(xymodem_handle_t handle, void *ctx, const char *name, uint64_t size, bool has_size)
{
    (void)handle;
    app_ctx_t *app = ctx;
    strncpy(app->name, name, sizeof(app->name) - 1);
    app->name[sizeof(app->name) - 1] = '\0';
    app->file_size = has_size ? size : 0;
    app->has_header = true;
    ESP_LOGI(TAG, "incoming file: \"%s\" (%" PRIu64 " bytes%s)",
             app->name, app->file_size, has_size ? "" : ", size unknown");
    return true;
}

/* ---- data source: data_source() supplies payload for the echo-back ---- */
static esp_err_t app_load(xymodem_handle_t handle, void *ctx, uint8_t *buf, size_t len, size_t *read_len)
{
    (void)handle;
    app_ctx_t *app = ctx;
    size_t remain = app->stored - app->read_pos;
    size_t n = remain < len ? remain : len;
    memcpy(buf, app->storage + app->read_pos, n);
    app->read_pos += n;
    *read_len = n;
    return ESP_OK;
}

/* ---- lifecycle events ---- */
static void app_event(xymodem_handle_t handle, void *ctx, xymodem_event_t event, xymodem_err_t arg)
{
    (void)handle; (void)ctx; (void)arg;
    switch (event) {
    case XYMODEM_EV_HANDSHAKE:
        ESP_LOGI(TAG, "handshake negotiated");
        break;
    case XYMODEM_EV_FILE_COMPLETE:
        ESP_LOGI(TAG, "file complete (batch continues)");
        break;
    case XYMODEM_EV_COMPLETE:
        ESP_LOGI(TAG, "transfer complete");
        break;
    case XYMODEM_EV_FAILED:
        ESP_LOGE(TAG, "transfer failed: %d", (int)arg);
        break;
    default:
        break;
    }
}

/* One handle, alternating sessions. Only the application's own bookkeeping
 * is reset here; the driver resets all protocol state in each start call. */
static void begin_recv(app_ctx_t *app)
{
    app->stored = 0;
    app->has_header = false;
    esp_err_t err = xymodem_start_recv(app->handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "xymodem_start_recv failed: %s", esp_err_to_name(err));
    }
}

static void begin_send(app_ctx_t *app, const xymodem_file_info_t *file)
{
    app->read_pos = 0;
    esp_err_t err = xymodem_start_send(app->handle, file);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "starting the send session failed: %s", esp_err_to_name(err));
    }
}

/* UART setup for the example's physical link. */
static esp_err_t uart_init(void)
{
    uart_config_t uart_cfg = {
        .baud_rate = CONFIG_EXAMPLE_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_param_config(UART_PORT, &uart_cfg), TAG, "uart_param_config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(UART_PORT, CONFIG_EXAMPLE_UART_TX_GPIO,
                                     CONFIG_EXAMPLE_UART_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "uart_set_pin failed");
    ESP_RETURN_ON_ERROR(uart_driver_install(UART_PORT, UART_RX_BUF, UART_TX_BUF, 0, NULL, 0),
                        TAG, "uart_driver_install failed");
    return ESP_OK;
}

void app_main(void)
{
    /* Heap-allocated once and never freed: the ~8 KB receive storage does not
     * fit a default task stack, and the echo server runs for the lifetime of
     * the device anyway. */
    app_ctx_t *app = calloc(1, sizeof(*app));
    if (app == NULL) {
        ESP_LOGE(TAG, "no memory for the application context");
        return;
    }

    /* One handle, both directions: data_sink is the receive path, data_source
     * the send path. There is no protocol setting -- a block 0 is recognised
     * on receive and requested on send through the file argument. */
    xymodem_config_t cfg = {
        .core = XYMODEM_CORE_CONFIG_DEFAULT(),
        .emit = app_emit,
        .data_sink = app_store,
        .data_source = app_load,
        .on_file = app_on_file,
        .on_event = app_event,
        .user_ctx = app,
    };
    cfg.core.check_mode = XYMODEM_CHECK_AUTO;

    ESP_ERROR_CHECK(uart_init());
    ESP_ERROR_CHECK(xymodem_new(&cfg, &app->handle));

    ESP_LOGI(TAG, "Waiting for an X/YMODEM upload...");
    /* After COMPLETE, xymodem_role() is NONE: the application owns the next
     * phase. `need_end` means the echo carried a block 0, so a following
     * empty-name xymodem_start_send() closes the YMODEM batch for the host. */
    bool receiving = true;
    bool need_end = false;
    ESP_ERROR_CHECK(xymodem_start_recv(app->handle));

    /* One loop: the uart_read_bytes timeout is the poll period. A UHCI ISR
     * would call xymodem_on_receive() itself and this loop would only poll. */
    uint8_t wire[128];
    TickType_t delay = pdMS_TO_TICKS(POLL_PERIOD_MS);
    if (delay == 0) {
        delay = 1; /* configTICK_RATE_HZ=100 would make 5 ms a zero tick */
    }
    while (1) {
        int n = uart_read_bytes(UART_PORT, wire, sizeof(wire), delay);
        if (n > 0) {
            xymodem_on_receive(app->handle, wire, (size_t)n);
        }
        xymodem_poll(app->handle);
        xymodem_status_t st = xymodem_status(app->handle);
        if (st == XYMODEM_ST_COMPLETE && receiving) {
            ESP_LOGI(TAG, "Received %u bytes, echoing back", (unsigned)app->stored);
            vTaskDelay(pdMS_TO_TICKS(100)); /* let the final ACK drain */
            if (app->has_header) {
                xymodem_file_info_t file = { .name = app->name, .size = app->file_size };
                begin_send(app, &file);
                need_end = true;
            } else {
                begin_send(app, NULL); /* classic, header-less echo */
            }
            receiving = false;
            /* Armed as sender: the peer's 'C'/NAK is now consumed. Logged so
             * the host test can start recv() without losing the first request. */
            ESP_LOGI(TAG, "Echo sender ready");
        } else if (st == XYMODEM_ST_COMPLETE && need_end) {
            /* Several start_send calls == a batch: this empty-name session
             * sends the block 0 that tells the host "no more files". */
            xymodem_file_info_t end = { .name = "", .size = 0 };
            begin_send(app, &end);
            need_end = false;
        } else if (st == XYMODEM_ST_COMPLETE) {
            ESP_LOGI(TAG, "Echo done, waiting for the next upload");
            vTaskDelay(pdMS_TO_TICKS(500));
            begin_recv(app);
            receiving = true;
        } else if (st == XYMODEM_ST_FAILED) {
            ESP_LOGE(TAG, "Transfer failed (%d), restarting", (int)xymodem_error(app->handle));
            vTaskDelay(pdMS_TO_TICKS(500));
            begin_recv(app);
            receiving = true;
            need_end = false;
        }
    }
}
