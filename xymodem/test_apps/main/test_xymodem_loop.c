/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#include "test_xymodem_loop.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "unity.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if SOC_UART_HP_NUM < 2
#error "Need two HP UART ports for the loopback pair (console is on USB Serial/JTAG)"
#endif

#define SENDER_UART   ((uart_port_t)CONFIG_XYMODEM_TEST_SENDER_UART)
#define RECEIVER_UART ((uart_port_t)CONFIG_XYMODEM_TEST_RECEIVER_UART)
#define GPIO_A        CONFIG_XYMODEM_TEST_GPIO_A
#define GPIO_B        CONFIG_XYMODEM_TEST_GPIO_B
#define BAUD          CONFIG_XYMODEM_TEST_UART_BAUD

#define WAIT_TIMEOUT_MS 10000
#define TASK_STACK      4096
#define MAX_FILES       4

typedef struct {
    uart_port_t port;
    xymodem_handle_t handle;
} loop_end_t;

static loop_end_t s_send;
static loop_end_t s_recv;
static bool s_open;
static volatile bool s_stop;

static uint8_t s_tx[XYMODEM_LOOP_RX_CAP];
static uint8_t s_rx[XYMODEM_LOOP_RX_CAP];
static size_t s_tx_len;
static size_t s_tx_pos;
static size_t s_rx_len;

static char s_file_name[MAX_FILES][32];
static size_t s_file_size[MAX_FILES];
static bool s_file_has_size[MAX_FILES];
static int s_file_count;

static StackType_t s_pump0_stack[TASK_STACK / sizeof(StackType_t)];
static StackType_t s_pump1_stack[TASK_STACK / sizeof(StackType_t)];
static StackType_t s_poll_stack[TASK_STACK / sizeof(StackType_t)];
static StaticTask_t s_pump0_tcb;
static StaticTask_t s_pump1_tcb;
static StaticTask_t s_poll_tcb;
static TaskHandle_t s_pump0;
static TaskHandle_t s_pump1;
static TaskHandle_t s_poll;

static TickType_t ticks_ms(uint32_t ms)
{
    TickType_t t = pdMS_TO_TICKS(ms);
    return t > 0 ? t : 1;
}

static void fill_pattern(uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(i * 7u + 3u);
    }
}

static esp_err_t emit_uart(xymodem_handle_t handle, void *ctx, const uint8_t *data, size_t len)
{
    (void)handle;
    loop_end_t *end = ctx;
    int n = uart_write_bytes(end->port, (const char *)data, len);
    return n == (int)len ? ESP_OK : ESP_FAIL;
}

static esp_err_t recv_sink(xymodem_handle_t handle, void *ctx, const uint8_t *data, size_t len)
{
    (void)handle;
    (void)ctx;
    if (s_rx_len + len > sizeof(s_rx)) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(s_rx + s_rx_len, data, len);
    s_rx_len += len;
    return ESP_OK;
}

static bool recv_on_file(xymodem_handle_t handle, void *ctx, const char *name, uint64_t size, bool has_size)
{
    (void)handle;
    (void)ctx;
    if (s_file_count >= MAX_FILES) {
        return false;
    }
    int i = s_file_count++;
    strlcpy(s_file_name[i], name, sizeof(s_file_name[i]));
    s_file_size[i] = has_size ? (size_t)size : 0;
    s_file_has_size[i] = has_size;
    return true;
}

static esp_err_t send_source(xymodem_handle_t handle, void *ctx, uint8_t *buf, size_t len, size_t *read_len)
{
    (void)handle;
    (void)ctx;
    size_t remain = s_tx_len - s_tx_pos;
    size_t n = remain < len ? remain : len;
    memcpy(buf, s_tx + s_tx_pos, n);
    s_tx_pos += n;
    *read_len = n;
    return ESP_OK;
}

static void pump_task(void *arg)
{
    loop_end_t *end = arg;
    uint8_t buf[256];
    while (!s_stop) {
        int n = uart_read_bytes(end->port, buf, sizeof(buf), ticks_ms(20));
        if (n > 0) {
            xymodem_on_receive(end->handle, buf, (size_t)n);
        }
    }
    vTaskSuspend(NULL);
}

static void poll_task(void *arg)
{
    (void)arg;
    TickType_t delay = ticks_ms(5);
    while (!s_stop) {
        if (s_send.handle) {
            xymodem_poll(s_send.handle);
        }
        if (s_recv.handle) {
            xymodem_poll(s_recv.handle);
        }
        vTaskDelay(delay);
    }
    vTaskSuspend(NULL);
}

static void wait_status(xymodem_handle_t handle, xymodem_status_t want, const char *what)
{
    int waited = 0;
    while (waited < WAIT_TIMEOUT_MS) {
        xymodem_status_t st = xymodem_status(handle);
        if (st == want) {
            return;
        }
        if (st == XYMODEM_ST_FAILED) {
            char msg[96];
            snprintf(msg, sizeof(msg), "%s (err %d)", what, (int)xymodem_error(handle));
            TEST_FAIL_MESSAGE(msg);
        }
        vTaskDelay(ticks_ms(10));
        waited += 10;
    }
    {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s (status %d)", what, (int)xymodem_status(handle));
        TEST_FAIL_MESSAGE(msg);
    }
}

void xymodem_loop_uart_init(void)
{
    static bool installed = false;
    if (installed) {
        return;
    }

    TEST_ASSERT_NOT_EQUAL(SENDER_UART, RECEIVER_UART);
    TEST_ASSERT_NOT_EQUAL(GPIO_A, GPIO_B);

    uart_config_t cfg = {
        .baud_rate = BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    TEST_ESP_OK(uart_driver_install(SENDER_UART, 2048, 2048, 0, NULL, 0));
    TEST_ESP_OK(uart_param_config(SENDER_UART, &cfg));
    TEST_ESP_OK(uart_set_pin(SENDER_UART, GPIO_A, GPIO_B, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    TEST_ESP_OK(uart_driver_install(RECEIVER_UART, 2048, 2048, 0, NULL, 0));
    TEST_ESP_OK(uart_param_config(RECEIVER_UART, &cfg));
    TEST_ESP_OK(uart_set_pin(RECEIVER_UART, GPIO_B, GPIO_A, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    installed = true;
}

void xymodem_loop_flush(void)
{
    (void)uart_flush_input(SENDER_UART);
    (void)uart_flush_input(RECEIVER_UART);
}

void xymodem_loop_open(uint8_t check_mode, bool block_1k)
{
    TEST_ASSERT_FALSE_MESSAGE(s_open, "loop already open");
    memset(&s_send, 0, sizeof(s_send));
    memset(&s_recv, 0, sizeof(s_recv));
    s_send.port = SENDER_UART;
    s_recv.port = RECEIVER_UART;
    s_stop = false;
    s_tx_len = s_tx_pos = s_rx_len = 0;
    s_file_count = 0;

    xymodem_config_t send_cfg = {
        .core = XYMODEM_CORE_CONFIG_DEFAULT(),
        .emit = emit_uart,
        .data_source = send_source,
        .user_ctx = &s_send,
    };
    xymodem_config_t recv_cfg = {
        .core = XYMODEM_CORE_CONFIG_DEFAULT(),
        .emit = emit_uart,
        .data_sink = recv_sink,
        .on_file = recv_on_file,
        .user_ctx = &s_recv,
    };
    send_cfg.core.check_mode = check_mode;
    recv_cfg.core.check_mode = check_mode;
    send_cfg.core.block_1k = block_1k;
    /* Short enough that a 'C' dropped between YMODEM sessions is retried
     * well inside WAIT_TIMEOUT_MS; long enough to not NAK in-flight UART. */
    send_cfg.core.packet_timeout_ms = 200;
    recv_cfg.core.packet_timeout_ms = 200;
    send_cfg.core.response_timeout_ms = 2000;
    recv_cfg.core.response_timeout_ms = 2000;
    TEST_ESP_OK(xymodem_new(&send_cfg, &s_send.handle));
    TEST_ESP_OK(xymodem_new(&recv_cfg, &s_recv.handle));

    s_pump0 = xTaskCreateStatic(pump_task, "pump_send", TASK_STACK, &s_send, 5,
                                s_pump0_stack, &s_pump0_tcb);
    s_pump1 = xTaskCreateStatic(pump_task, "pump_recv", TASK_STACK, &s_recv, 5,
                                s_pump1_stack, &s_pump1_tcb);
    s_poll = xTaskCreateStatic(poll_task, "poll", TASK_STACK, NULL, 4,
                               s_poll_stack, &s_poll_tcb);
    TEST_ASSERT_NOT_NULL(s_pump0);
    TEST_ASSERT_NOT_NULL(s_pump1);
    TEST_ASSERT_NOT_NULL(s_poll);
    s_open = true;
}

void xymodem_loop_close(void)
{
    if (!s_open) {
        return;
    }
    s_stop = true;
    vTaskDelay(ticks_ms(80));
    if (s_pump0) {
        vTaskDelete(s_pump0);
        s_pump0 = NULL;
    }
    if (s_pump1) {
        vTaskDelete(s_pump1);
        s_pump1 = NULL;
    }
    if (s_poll) {
        vTaskDelete(s_poll);
        s_poll = NULL;
    }
    /* Idle frees the static TCBs; wait before the next xTaskCreateStatic. */
    vTaskDelay(ticks_ms(20));
    if (s_send.handle) {
        TEST_ESP_OK(xymodem_delete(s_send.handle));
        s_send.handle = NULL;
    }
    if (s_recv.handle) {
        TEST_ESP_OK(xymodem_delete(s_recv.handle));
        s_recv.handle = NULL;
    }
    s_open = false;
}

void xymodem_loop_start_recv(void)
{
    s_rx_len = 0;
    s_file_count = 0;
    TEST_ESP_OK(xymodem_start_recv(s_recv.handle));
}

void xymodem_loop_start_send(size_t len, const char *name)
{
    TEST_ASSERT_TRUE(len <= sizeof(s_tx));
    fill_pattern(s_tx, len);
    s_tx_len = len;
    s_tx_pos = 0;
    if (name) {
        xymodem_file_info_t file = { .name = name, .size = len };
        TEST_ESP_OK(xymodem_start_send(s_send.handle, &file));
    } else {
        TEST_ESP_OK(xymodem_start_send(s_send.handle, NULL));
    }
}

void xymodem_loop_wait_sender(void)
{
    wait_status(s_send.handle, XYMODEM_ST_COMPLETE, "sender did not complete");
}

void xymodem_loop_wait_both(void)
{
    wait_status(s_send.handle, XYMODEM_ST_COMPLETE, "sender did not complete");
    wait_status(s_recv.handle, XYMODEM_ST_COMPLETE, "receiver did not complete");
}

const uint8_t *xymodem_loop_tx(void)
{
    return s_tx;
}

const uint8_t *xymodem_loop_rx(void)
{
    return s_rx;
}

size_t xymodem_loop_rx_len(void)
{
    return s_rx_len;
}

int xymodem_loop_file_count(void)
{
    return s_file_count;
}

const char *xymodem_loop_file_name(int index)
{
    TEST_ASSERT_TRUE(index >= 0 && index < s_file_count);
    return s_file_name[index];
}

size_t xymodem_loop_file_size(int index)
{
    TEST_ASSERT_TRUE(index >= 0 && index < s_file_count);
    return s_file_size[index];
}

bool xymodem_loop_file_has_size(int index)
{
    TEST_ASSERT_TRUE(index >= 0 && index < s_file_count);
    return s_file_has_size[index];
}

size_t xymodem_loop_padded_len(size_t payload_len, bool block_1k)
{
    if (payload_len == 0) {
        return 0;
    }
    size_t n = 0;
    while (payload_len > 0) {
        size_t cap = block_1k ? 1024 : 128;
        size_t chunk = payload_len < cap ? payload_len : cap;
        n += (block_1k && chunk > 128) ? 1024 : 128;
        payload_len -= chunk;
    }
    return n;
}
