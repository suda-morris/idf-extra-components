/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "lz4file.h"
#include "unity.h"

TEST_CASE("lz4file roundtrip works with LittleFS VFS", "[lz4][littlefs]")
{
    enum { SOURCE_SIZE = 4096 };
    const char *path = "/littlefs/lz4file-test.lz4";
    uint8_t *source = malloc(SOURCE_SIZE);
    uint8_t *restored = malloc(SOURCE_SIZE);
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_NOT_NULL(restored);

    for (size_t i = 0; i < SOURCE_SIZE; i++) {
        source[i] = (uint8_t)((i * 37u) ^ (i >> 3));
    }

    // LittleFS is mounted before Unity starts, so its one-time initialization
    // allocations are outside the per-test memory-leak baseline.
    FILE *file = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(file);

    LZ4_writeFile_t *writer = NULL;
    LZ4F_preferences_t preferences = { 0 };
    preferences.frameInfo.blockSizeID = LZ4F_max64KB;
    preferences.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;

    // lz4file keeps one complete compressed block in memory. The 64 KiB
    // minimum block size can be unavailable as one contiguous block on a
    // fragmented heap, even when total free memory looks sufficient.
    const size_t file_buffer_size = LZ4F_compressBound(64 * 1024, &preferences);
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < file_buffer_size) {
        fclose(file);
        free(restored);
        free(source);
        TEST_IGNORE_MESSAGE("Not enough contiguous 8-bit heap for lz4file buffer");
    }

    const LZ4F_errorCode_t write_open_rc = LZ4F_writeOpen(&writer, file, &preferences);
    if (LZ4F_isError(write_open_rc)) {
        printf("LZ4F_writeOpen failed: %s\n", LZ4F_getErrorName(write_open_rc));
        fclose(file);
        free(restored);
        free(source);
        if (LZ4F_getErrorCode(write_open_rc) == LZ4F_ERROR_allocation_failed) {
            TEST_IGNORE_MESSAGE("Not enough contiguous heap for lz4file writer");
        }
        TEST_FAIL_MESSAGE("lz4file writer initialization failed");
    }
    TEST_ASSERT_NOT_NULL(writer);
    TEST_ASSERT_EQUAL_UINT32(SOURCE_SIZE, (uint32_t)LZ4F_write(writer, source, SOURCE_SIZE));
    TEST_ASSERT_FALSE(LZ4F_isError(LZ4F_writeClose(writer)));
    TEST_ASSERT_EQUAL(0, fclose(file));

    file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(file);

    LZ4_readFile_t *reader = NULL;
    const LZ4F_errorCode_t read_open_rc = LZ4F_readOpen(&reader, file);
    if (LZ4F_isError(read_open_rc)) {
        printf("LZ4F_readOpen failed: %s\n", LZ4F_getErrorName(read_open_rc));
        fclose(file);
        free(restored);
        free(source);
        if (LZ4F_getErrorCode(read_open_rc) == LZ4F_ERROR_allocation_failed) {
            TEST_IGNORE_MESSAGE("Not enough contiguous heap for lz4file reader");
        }
        TEST_FAIL_MESSAGE("lz4file reader initialization failed");
    }
    TEST_ASSERT_NOT_NULL(reader);
    const size_t read_size = LZ4F_read(reader, restored, SOURCE_SIZE);
    if (LZ4F_isError(read_size)) {
        printf("LZ4F_read failed: %s\n", LZ4F_getErrorName(read_size));
        LZ4F_readClose(reader);
        fclose(file);
        free(restored);
        free(source);
        if (LZ4F_getErrorCode(read_size) == LZ4F_ERROR_allocation_failed) {
            TEST_IGNORE_MESSAGE("Not enough contiguous heap for lz4file reader");
        }
        TEST_FAIL_MESSAGE("lz4file read failed");
    }
    TEST_ASSERT_EQUAL_UINT32(SOURCE_SIZE, (uint32_t)read_size);
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)LZ4F_read(reader, restored, 1));
    TEST_ASSERT_FALSE(LZ4F_isError(LZ4F_readClose(reader)));
    TEST_ASSERT_EQUAL(0, fclose(file));

    TEST_ASSERT_EQUAL_UINT8_ARRAY(source, restored, SOURCE_SIZE);
    TEST_ASSERT_EQUAL(0, remove(path));

    free(restored);
    free(source);
}
