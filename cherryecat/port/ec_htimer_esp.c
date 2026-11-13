/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* CherryECAT ESP-IDF high precision timer port (ec_port.h ec_htimer_* API).
 *
 * A hardware gptimer (1 MHz tick) arms a one-shot alarm per PDO cycle; its
 * ISR wakes a dedicated cycle task which runs the periodic process in task
 * context. Task context keeps the process legal for the ESP-IDF Ethernet
 * driver and for application PDO callbacks, while the alarm is anchored to
 * the previous alarm (not to the task wake-up time) so the long term cycle
 * rate stays exact and the wake-up jitter only shifts single cycles.
 *
 * The PDO callback registered in ec_slave_config_t runs in the cycle task
 * context; keep it short and non-blocking, as with upstream. */

#include "ec_master.h"

#include "esp_check.h"
#include "esp_timer.h"
#include "driver/gptimer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "ec_htimer";

#define EC_HTIMER_TICK_HZ      (1000 * 1000) /* 1 MHz: gptimer counts in microseconds. */
#define EC_HTIMER_PERIOD_MIN_US 1

static gptimer_handle_t s_gptimer = NULL;
static SemaphoreHandle_t s_cycle_sem = NULL;
static TaskHandle_t s_cycle_task = NULL;

static ec_htimer_cb s_cb = NULL;
static void *s_arg = NULL;

static volatile bool s_running = false;
static volatile uint32_t s_period_us = EC_HTIMER_PERIOD_MIN_US;
static volatile uint64_t s_last_alarm_us = 0;

static bool IRAM_ATTR ec_htimer_alarm_isr(gptimer_handle_t timer,
                                          const gptimer_alarm_event_data_t *edata,
                                          void *user_data)
{
    BaseType_t high_task_awoken = pdFALSE;
    (void)timer;
    (void)user_data;

    /* Anchor the next alarm on this alarm, so the cycle rate does not drift
       by the wake-up latency of the cycle task. */
    s_last_alarm_us = edata->alarm_value;

    xSemaphoreGiveFromISR(s_cycle_sem, &high_task_awoken);
    return high_task_awoken == pdTRUE;
}

static void ec_htimer_cycle_task(void *arg)
{
    (void)arg;

    while (1) {
        (void)xSemaphoreTake(s_cycle_sem, portMAX_DELAY);
        if (!s_running) {
            continue;
        }

        if (s_cb) {
            /* The periodic process may adjust the period through
               ec_htimer_update(); read it after the callback returns. */
            s_cb(s_arg);
        }

        uint64_t next_alarm_us = s_last_alarm_us + s_period_us;
        gptimer_alarm_config_t alarm_config = {
            .alarm_count = next_alarm_us,
            .reload_count = 0,
            .flags.auto_reload_on_alarm = false,
        };
        /* An alarm in the past triggers immediately, which is the desired
           catch-up behavior when a cycle overran its budget. */
        (void)gptimer_set_alarm_action(s_gptimer, &alarm_config);
    }
}

static esp_err_t ec_htimer_resources_create(void)
{
    esp_err_t ret = ESP_OK;

    if (s_gptimer != NULL) {
        return ESP_OK;
    }

    s_cycle_sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_cycle_sem != NULL, ESP_ERR_NO_MEM, TAG,
                        "Failed to create the cycle task semaphore");

    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = EC_HTIMER_TICK_HZ,
    };
    ESP_GOTO_ON_ERROR(gptimer_new_timer(&timer_config, &s_gptimer), err, TAG,
                      "Failed to create the gptimer");

    gptimer_event_callbacks_t callbacks = {
        .on_alarm = ec_htimer_alarm_isr,
    };
    ESP_GOTO_ON_ERROR(gptimer_register_event_callbacks(s_gptimer, &callbacks, NULL), err, TAG,
                      "Failed to register the gptimer alarm callback");

    BaseType_t core = (CONFIG_CHERRYECAT_CYCLE_TASK_CORE < 0) ? tskNO_AFFINITY
                      : (BaseType_t)CONFIG_CHERRYECAT_CYCLE_TASK_CORE;
    BaseType_t create_ret = xTaskCreatePinnedToCore(ec_htimer_cycle_task, "ec_cycle",
                                                    CONFIG_CHERRYECAT_CYCLE_TASK_STACKSIZE,
                                                    NULL,
                                                    CONFIG_CHERRYECAT_CYCLE_TASK_PRIORITY,
                                                    &s_cycle_task, core);
    ESP_GOTO_ON_FALSE(create_ret == pdPASS, ESP_ERR_NO_MEM, err, TAG,
                      "Failed to create the cycle task");

    ESP_GOTO_ON_ERROR(gptimer_enable(s_gptimer), err, TAG,
                      "Failed to enable the gptimer");

    return ESP_OK;

err:
    if (s_cycle_task != NULL) {
        vTaskDelete(s_cycle_task);
        s_cycle_task = NULL;
    }
    if (s_gptimer != NULL) {
        (void)gptimer_del_timer(s_gptimer);
        s_gptimer = NULL;
    }
    if (s_cycle_sem != NULL) {
        vSemaphoreDelete(s_cycle_sem);
        s_cycle_sem = NULL;
    }
    return ret;
}

void ec_htimer_start(uint32_t us, ec_htimer_cb cb, void *arg)
{
    if (ec_htimer_resources_create() != ESP_OK) {
        EC_LOG_ERR("Failed to create the high precision timer resources\n");
        return;
    }

    s_cb = cb;
    s_arg = arg;
    s_period_us = (us > EC_HTIMER_PERIOD_MIN_US) ? us : EC_HTIMER_PERIOD_MIN_US;

    uint64_t now_us = 0;
    (void)gptimer_get_raw_count(s_gptimer, &now_us);

    gptimer_alarm_config_t alarm_config = {
        .alarm_count = now_us + s_period_us,
        .reload_count = 0,
        .flags.auto_reload_on_alarm = false,
    };
    (void)gptimer_set_alarm_action(s_gptimer, &alarm_config);

    s_running = true;
    (void)gptimer_start(s_gptimer);
}

void ec_htimer_stop(void)
{
    if (s_gptimer == NULL) {
        return;
    }

    s_running = false;
    (void)gptimer_stop(s_gptimer);
}

EC_FAST_CODE_SECTION void ec_htimer_update(uint32_t ns)
{
    uint32_t period_us = (uint32_t)((ns + 999) / 1000);

    if (period_us < EC_HTIMER_PERIOD_MIN_US) {
        period_us = EC_HTIMER_PERIOD_MIN_US;
    }
    s_period_us = period_us;
}
