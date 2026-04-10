/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "esp_err.h"
#include "lz4frame.h"

#include "lz4_example.h"

#define LZ4_CHECK(condition) ESP_ERROR_CHECK((condition) ? ESP_OK : ESP_FAIL)
#define LZ4F_CHECK(code) ESP_ERROR_CHECK(LZ4F_isError(code) ? ESP_FAIL : ESP_OK)

void run_stream_example(void)
{
    enum {
        SAMPLE_COUNT = 4096,
        INPUT_CHUNK = 7,
        OUTPUT_CHUNK = 31,
        MAX_DECOMPRESS_STEPS = 4096,
    };
    const size_t source_size = SAMPLE_COUNT * sizeof(int16_t);

    printf("\nFrame streaming API example\n");

    uint8_t *source = malloc(source_size);
    LZ4_CHECK(source != NULL);
    fill_pcm_like_samples((int16_t *)source, SAMPLE_COUNT);

    // Independent 64 KiB blocks keep Frame scratch modest on typical ESP
    // targets. Linked blocks and larger sizes need more RAM; see lz4frame.h.
    LZ4F_preferences_t preferences = { 0 };
    preferences.frameInfo.blockSizeID = LZ4F_max64KB;
    preferences.frameInfo.blockMode = LZ4F_blockIndependent;
    preferences.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
    const size_t frame_capacity = LZ4F_compressFrameBound(source_size, &preferences);
    LZ4_CHECK(frame_capacity > 0);

    uint8_t *frame = malloc(frame_capacity);
    uint8_t *restored = malloc(source_size);
    LZ4_CHECK((frame != NULL) && (restored != NULL));

    const size_t frame_size = LZ4F_compressFrame(frame, frame_capacity, source, source_size, &preferences);
    LZ4F_CHECK(frame_size);

    LZ4F_dctx *decompression_context = NULL;
    LZ4F_CHECK(LZ4F_createDecompressionContext(&decompression_context, LZ4F_VERSION));

    size_t source_offset = 0;
    size_t restored_offset = 0;
    size_t result = 1;
    // Feed and receive small chunks to demonstrate repeated decompression calls.
    for (size_t step = 0; step < MAX_DECOMPRESS_STEPS && result != 0; step++) {
        size_t input_size = MIN(frame_size - source_offset, INPUT_CHUNK);
        size_t output_size = MIN(source_size - restored_offset, OUTPUT_CHUNK);

        result = LZ4F_decompress(decompression_context,
                                 restored + restored_offset, &output_size,
                                 frame + source_offset, &input_size, NULL);
        LZ4F_CHECK(result);
        source_offset += input_size;
        restored_offset += output_size;
        LZ4_CHECK((input_size > 0) || (output_size > 0) || (result == 0));
    }

    // A zero return value means that the complete Frame has been decoded.
    LZ4_CHECK((result == 0) && (source_offset == frame_size) &&
              (restored_offset == source_size) &&
              (memcmp(source, restored, source_size) == 0));
    LZ4F_CHECK(LZ4F_freeDecompressionContext(decompression_context));

    printf("source_size=%zu frame_size=%zu verify=true\n", source_size, frame_size);

    free(restored);
    free(frame);
    free(source);
}
