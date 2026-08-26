/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void run_block_example(void);
void run_stream_example(void);
void fill_pcm_like_samples(int16_t *samples, size_t sample_count);

#ifdef __cplusplus
}
#endif
