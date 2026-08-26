/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lz4_example.h"

void app_main(void)
{
    // Start with the simplest complete-buffer API, then show streaming frames.
    run_block_example();
    run_stream_example();
}
