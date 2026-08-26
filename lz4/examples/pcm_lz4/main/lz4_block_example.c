/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "lz4.h"

#include "lz4_example.h"

#define LZ4_CHECK(condition) ESP_ERROR_CHECK((condition) ? ESP_OK : ESP_FAIL)

void run_block_example(void)
{
    enum { SAMPLE_COUNT = 4096 };
    const size_t source_size = SAMPLE_COUNT * sizeof(int16_t);

    printf("\nBlock API example\n");

    char *source = malloc(source_size);
    LZ4_CHECK(source != NULL);
    fill_pcm_like_samples((int16_t *)source, SAMPLE_COUNT);

    const int compressed_capacity = LZ4_compressBound((int)source_size);
    LZ4_CHECK(compressed_capacity > 0);

    char *compressed = malloc(compressed_capacity);
    char *restored = malloc(source_size);
    LZ4_CHECK((compressed != NULL) && (restored != NULL));

    // Reuse this state across repeated compress calls. LZ4_compress_default()
    // allocates a hash table on every call, which is expensive on ESP-IDF.
    LZ4_stream_t *state = LZ4_createStream();
    LZ4_CHECK(state != NULL);

    const int compressed_size = LZ4_compress_fast_extState(state, source, compressed,
                                                           (int)source_size, compressed_capacity, 1);
    LZ4_CHECK(compressed_size > 0);

    // Block data does not contain the original size, so pass it separately.
    const int restored_size = LZ4_decompress_safe(compressed, restored, compressed_size, (int)source_size);
    LZ4_CHECK((restored_size == (int)source_size) &&
              (memcmp(source, restored, source_size) == 0));

    printf("source_size=%u compressed_size=%d verify=true\n",
           (unsigned)source_size, compressed_size);

    LZ4_freeStream(state);
    free(restored);
    free(compressed);
    free(source);
}
