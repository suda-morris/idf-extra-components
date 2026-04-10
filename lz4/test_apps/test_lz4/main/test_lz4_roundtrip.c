/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

#include "unity.h"
#include "unity_test_runner.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

#include "lz4.h"
#include "lz4frame.h"
#include "lz4hc.h"

static void fill_deterministic(uint8_t *buf, size_t len)
{
    uint32_t state = 0xA5A5A5A5u;

    for (size_t i = 0; i < len; i++) {
        state = (state * 1664525u) + 1013904223u;
        buf[i] = (uint8_t)((state >> 24) ^ (uint32_t)i ^ ((uint32_t)i << 3));
    }
}

static size_t lz4_largest_heap_block(void)
{
#if CONFIG_LZ4_USE_PSRAM
    return MAX(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
               heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#else
    return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
#endif
}

TEST_CASE("lz4 block roundtrip returns original payload", "[lz4]")
{
    enum { SRC_SIZE = 4096 };

    uint8_t *src = malloc(SRC_SIZE);
    TEST_ASSERT_NOT_NULL(src);
    fill_deterministic(src, SRC_SIZE);

    const int max_compressed_size = LZ4_compressBound(SRC_SIZE);
    TEST_ASSERT_GREATER_THAN(0, max_compressed_size);

    char *compressed = malloc((size_t)max_compressed_size);
    TEST_ASSERT_NOT_NULL(compressed);

    const int compressed_size = LZ4_compress_default((const char *)src, compressed, SRC_SIZE, max_compressed_size);
    TEST_ASSERT_GREATER_THAN(0, compressed_size);

    uint8_t *decompressed = malloc(SRC_SIZE);
    TEST_ASSERT_NOT_NULL(decompressed);

    const int decompressed_size = LZ4_decompress_safe(compressed, (char *)decompressed, compressed_size, SRC_SIZE);
    TEST_ASSERT_EQUAL_INT(SRC_SIZE, decompressed_size);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src, decompressed, SRC_SIZE);

    free(decompressed);
    free(compressed);
    free(src);
}

TEST_CASE("lz4 hc roundtrip returns original payload", "[lz4]")
{
    enum { SRC_SIZE = 8192 };

    uint8_t *src = malloc(SRC_SIZE);
    TEST_ASSERT_NOT_NULL(src);
    fill_deterministic(src, SRC_SIZE);

    const int max_compressed_size = LZ4_compressBound(SRC_SIZE);
    TEST_ASSERT_GREATER_THAN(0, max_compressed_size);

    char *compressed = malloc((size_t)max_compressed_size);
    TEST_ASSERT_NOT_NULL(compressed);

    // HC uses a large workspace (about 256 KiB with the upstream defaults).
    // With CONFIG_LZ4_USE_PSRAM that allocation prefers PSRAM (it is above
    // the 32 KiB split) and falls back to internal 8-bit RAM. Some targets,
    // including ESP32-C3 configurations, may not have a sufficiently large
    // contiguous block after the test framework starts. Treat that as a
    // resource limitation rather than a codec failure.
    const size_t largest_block = lz4_largest_heap_block();
    if (largest_block < (size_t)LZ4_sizeofStateHC()) {
        free(compressed);
        free(src);
        TEST_IGNORE_MESSAGE("Not enough contiguous heap for LZ4 HC workspace");
    }

    const int compressed_size = LZ4_compress_HC((const char *)src, compressed, SRC_SIZE, max_compressed_size, LZ4HC_CLEVEL_DEFAULT);
    TEST_ASSERT_GREATER_THAN(0, compressed_size);

    uint8_t *decompressed = malloc(SRC_SIZE);
    TEST_ASSERT_NOT_NULL(decompressed);

    const int decompressed_size = LZ4_decompress_safe(compressed, (char *)decompressed, compressed_size, SRC_SIZE);
    TEST_ASSERT_EQUAL_INT(SRC_SIZE, decompressed_size);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src, decompressed, SRC_SIZE);

    free(decompressed);
    free(compressed);
    free(src);
}

TEST_CASE("lz4 frame supports streaming compression and decompression", "[lz4]")
{
    enum { SRC_SIZE = 6143 };
    enum { COMPRESS_CHUNK = 13, DECOMPRESS_CHUNK = 31 };
    enum { MAX_DECOMPRESS_STEPS = 4096 };
    LZ4F_preferences_t prefs = { 0 };
    // Independent 64 KiB blocks keep Frame scratch around one block plus the
    // Block state, instead of ~192 KiB with upstream linked-block defaults.
    // autoFlush stays disabled, so small chunks accumulate in the context
    // and compressUpdate() may legitimately write zero bytes.
    prefs.frameInfo.blockSizeID = LZ4F_max64KB;
    prefs.frameInfo.blockMode = LZ4F_blockIndependent;
    prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;

    uint8_t *src = malloc(SRC_SIZE);
    TEST_ASSERT_NOT_NULL(src);
    fill_deterministic(src, SRC_SIZE);

    const size_t frame_capacity = LZ4F_compressFrameBound(SRC_SIZE, &prefs);
    TEST_ASSERT_GREATER_THAN(0, (int)frame_capacity);

    uint8_t *frame = malloc(frame_capacity);
    uint8_t *decompressed = malloc(SRC_SIZE);
    TEST_ASSERT_NOT_NULL(frame);
    TEST_ASSERT_NOT_NULL(decompressed);

    const size_t frame_scratch = (64 * 1024) + (size_t)LZ4_sizeofState();
    if (lz4_largest_heap_block() < frame_scratch) {
        free(decompressed);
        free(frame);
        free(src);
        TEST_IGNORE_MESSAGE("Not enough contiguous heap for LZ4 Frame scratch");
    }

    // Stream the source through the compressor in small chunks.
    LZ4F_cctx *cctx = NULL;
    const LZ4F_errorCode_t create_rc = LZ4F_createCompressionContext(&cctx, LZ4F_VERSION);
    TEST_ASSERT_FALSE(LZ4F_isError(create_rc));
    TEST_ASSERT_NOT_NULL(cctx);

    size_t frame_size = LZ4F_compressBegin(cctx, frame, frame_capacity, &prefs);
    TEST_ASSERT_FALSE(LZ4F_isError(frame_size));

    size_t src_offset = 0;
    while (src_offset < SRC_SIZE) {
        const size_t chunk = MIN(SRC_SIZE - src_offset, (size_t)COMPRESS_CHUNK);
        const size_t remaining = frame_capacity - frame_size;
        // The whole output accumulates in one buffer sized once with
        // LZ4F_compressFrameBound(SRC_SIZE). That total bound covers every
        // intermediate state of compressBegin/compressUpdate/compressEnd,
        // so per-invocation capacity checks are not needed here.

        const size_t written = LZ4F_compressUpdate(cctx,
                                                   frame + frame_size, remaining,
                                                   src + src_offset, chunk,
                                                   NULL);
        TEST_ASSERT_FALSE(LZ4F_isError(written));
        frame_size += written;
        src_offset += chunk;
    }

    const size_t end_size = LZ4F_compressEnd(cctx,
                                             frame + frame_size,
                                             frame_capacity - frame_size,
                                             NULL);
    TEST_ASSERT_FALSE(LZ4F_isError(end_size));
    frame_size += end_size;

    TEST_ASSERT_FALSE(LZ4F_isError(LZ4F_freeCompressionContext(cctx)));

    // Feed the produced frame back through the decompressor in small chunks.
    LZ4F_dctx *dctx = NULL;
    TEST_ASSERT_FALSE(LZ4F_isError(LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION)));
    TEST_ASSERT_NOT_NULL(dctx);

    size_t consumed_offset = 0;
    size_t restored_offset = 0;
    size_t rc = 1;
    for (size_t step = 0; step < MAX_DECOMPRESS_STEPS && rc != 0; step++) {
        size_t input_size = MIN(frame_size - consumed_offset, (size_t)DECOMPRESS_CHUNK);
        size_t output_size = MIN(SRC_SIZE - restored_offset, (size_t)DECOMPRESS_CHUNK);

        rc = LZ4F_decompress(dctx, decompressed + restored_offset, &output_size,
                             frame + consumed_offset, &input_size, NULL);
        TEST_ASSERT_FALSE(LZ4F_isError(rc));
        consumed_offset += input_size;
        restored_offset += output_size;
        if (rc != 0) {
            TEST_ASSERT_TRUE((input_size > 0) || (output_size > 0));
        }
    }

    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)rc);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)frame_size, (uint32_t)consumed_offset);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)SRC_SIZE, (uint32_t)restored_offset);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src, decompressed, SRC_SIZE);
    TEST_ASSERT_FALSE(LZ4F_isError(LZ4F_freeDecompressionContext(dctx)));

    free(decompressed);
    free(frame);
    free(src);
}

TEST_CASE("lz4 frame decompress reports error on corrupted payload", "[lz4]")
{
    enum { SRC_SIZE = 6143 };
    enum { MAX_DECOMPRESS_STEPS = 64 };
    LZ4F_preferences_t prefs = { 0 };
    prefs.frameInfo.blockSizeID = LZ4F_max64KB;
    prefs.frameInfo.blockMode = LZ4F_blockIndependent;
    prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;

    uint8_t *src = malloc(SRC_SIZE);
    TEST_ASSERT_NOT_NULL(src);
    fill_deterministic(src, SRC_SIZE);

    const size_t frame_capacity = LZ4F_compressFrameBound(SRC_SIZE, &prefs);
    TEST_ASSERT_GREATER_THAN(0, (int)frame_capacity);

    uint8_t *frame = malloc(frame_capacity);
    TEST_ASSERT_NOT_NULL(frame);

    const size_t frame_size = LZ4F_compressFrame(frame, frame_capacity, src, SRC_SIZE, &prefs);
    TEST_ASSERT_FALSE(LZ4F_isError(frame_size));
    TEST_ASSERT_GREATER_THAN(0, (int)frame_size);

    const size_t corrupt_index = frame_size / 2;
    TEST_ASSERT_TRUE(corrupt_index < (frame_size - 4));
    frame[corrupt_index] ^= 0x01;

    uint8_t *decompressed = malloc(SRC_SIZE);
    TEST_ASSERT_NOT_NULL(decompressed);

    LZ4F_dctx *dctx = NULL;
    const LZ4F_errorCode_t create_rc = LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION);
    TEST_ASSERT_FALSE(LZ4F_isError(create_rc));
    TEST_ASSERT_NOT_NULL(dctx);

    size_t src_offset = 0;
    size_t dst_offset = 0;
    LZ4F_errorCode_t rc = 1;

    for (size_t step = 0; step < MAX_DECOMPRESS_STEPS; step++) {
        size_t src_size = frame_size - src_offset;
        size_t dst_size = SRC_SIZE - dst_offset;

        rc = LZ4F_decompress(dctx,
                             decompressed + dst_offset, &dst_size,
                             frame + src_offset, &src_size,
                             NULL);

        src_offset += src_size;
        dst_offset += dst_size;

        if (LZ4F_isError(rc)) {
            break;
        }

        if (rc == 0) {
            break;
        }

        TEST_ASSERT_TRUE((src_size > 0) || (dst_size > 0));
    }

    TEST_ASSERT_TRUE(LZ4F_isError(rc));

    const LZ4F_errorCode_t free_rc = LZ4F_freeDecompressionContext(dctx);
    TEST_ASSERT_FALSE(LZ4F_isError(free_rc));

    free(decompressed);
    free(frame);
    free(src);
}

TEST_CASE("lz4 block decompress fails when destination is too small", "[lz4]")
{
    enum { SRC_SIZE = 1024 };

    uint8_t *src = malloc(SRC_SIZE);
    TEST_ASSERT_NOT_NULL(src);
    fill_deterministic(src, SRC_SIZE);

    const int max_compressed_size = LZ4_compressBound(SRC_SIZE);
    TEST_ASSERT_GREATER_THAN(0, max_compressed_size);

    char *compressed = malloc((size_t)max_compressed_size);
    TEST_ASSERT_NOT_NULL(compressed);

    const int compressed_size = LZ4_compress_default((const char *)src, compressed, SRC_SIZE, max_compressed_size);
    TEST_ASSERT_GREATER_THAN(0, compressed_size);

    uint8_t *tiny_dst = malloc(SRC_SIZE - 1);
    TEST_ASSERT_NOT_NULL(tiny_dst);

    const int decompressed_size = LZ4_decompress_safe(compressed, (char *)tiny_dst, compressed_size, SRC_SIZE - 1);
    TEST_ASSERT_TRUE(decompressed_size < 0);

    free(tiny_dst);
    free(compressed);
    free(src);
}
