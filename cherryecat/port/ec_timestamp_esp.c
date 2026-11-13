/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* CherryECAT ESP-IDF timestamp port (CONFIG_EC_TIMESTAMP_CUSTOM).
 *
 * esp_timer provides a 64-bit monotonic clock with microsecond resolution on
 * every chip (both cores, no wrap-around). The EtherCAT timestamps therefore
 * carry a 1 us resolution; keep the CPU frequency fixed (no power management
 * scaling) when aiming for tight DC synchronization. */

#include "ec_master.h"

#include "esp_timer.h"
#include "esp_rom_sys.h"

void ec_timestamp_init(void)
{
    /* esp_timer is ready before the application runs; nothing to set up. */
}

uint64_t ec_timestamp_get_time_ns(void)
{
    return (uint64_t)esp_timer_get_time() * 1000ULL;
}

uint32_t ec_get_cpu_frequency(void)
{
    return esp_rom_get_cpu_ticks_per_us() * 1000000U;
}
