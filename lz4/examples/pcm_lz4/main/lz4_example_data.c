/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lz4_example.h"

/* Generate repeatable, locally correlated samples without external input. */
void fill_pcm_like_samples(int16_t *samples, size_t sample_count)
{
    for (size_t i = 0; i < sample_count; i++) {
        // Repeat a smooth waveform with a slowly changing offset. This keeps
        // the input realistic enough for a raw audio example while providing
        // repeated byte patterns that make compression visible.
        const int32_t phase = (int32_t)(i % 256u);
        const int32_t triangle = (phase < 128) ? phase : (255 - phase);
        const int32_t offset = (int32_t)((i / 256u) % 4u) * 8;
        int32_t sample = (triangle - 64) * 256 + offset;

        if (sample > INT16_MAX) {
            sample = INT16_MAX;
        } else if (sample < INT16_MIN) {
            sample = INT16_MIN;
        }
        samples[i] = (int16_t)sample;
    }
}
