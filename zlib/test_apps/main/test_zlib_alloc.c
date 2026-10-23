/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "sdkconfig.h"
#include "unity.h"
#include "zlib.h"

#define INTERNAL_8BIT (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define PSRAM_8BIT (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

/* The component's own split between the two regions, private to zlib (see
 * zlib_memory.c). Kept in sync here so the placement checks below stay
 * meaningful. */
#define COMPONENT_PSRAM_THRESHOLD (16 * 1024)

static void fill_deterministic(uint8_t *buf, size_t len)
{
    uint32_t state = 0x5A5A5A5Au;

    for (size_t i = 0; i < len; i++) {
        state = (state * 1664525u) + 1013904223u;
        buf[i] = (uint8_t)((state >> 24) ^ (uint32_t)i);
    }
}

/*
 * A private allocator, to prove the component's default does not override one
 * that the application installs itself. zlib documents that a zalloc/zfree
 * pair set before init is used as-is.
 */
static unsigned custom_alloc_calls;
static unsigned custom_free_calls;

static voidpf custom_zalloc(voidpf opaque, unsigned items, unsigned size)
{
    (void)opaque;
    custom_alloc_calls++;
    return malloc((size_t)items * size);
}

static void custom_zfree(voidpf opaque, voidpf addr)
{
    (void)opaque;
    custom_free_calls++;
    free(addr);
}

#if CONFIG_ZLIB_PREFER_PSRAM
static bool psram_usable(void)
{
    return esp_psram_is_initialized() && heap_caps_get_total_size(PSRAM_8BIT) > 0;
}
#endif

/*
 * Everything zlib allocates is either big enough to be worth moving to PSRAM
 * or small enough to be worth keeping in fast internal RAM. Both are checked
 * by watching how much of each region is free before and after, which needs
 * no assumptions about which region a given pointer ended up in.
 */
TEST_CASE("a small zlib request stays in internal RAM", "[zlib][alloc]")
{
#if CONFIG_ZLIB_PREFER_PSRAM
    const size_t before = heap_caps_get_free_size(INTERNAL_8BIT);

    void *ptr = heap_caps_malloc_prefer(256, 2, INTERNAL_8BIT, PSRAM_8BIT);
    TEST_ASSERT_NOT_NULL(ptr);
    memset(ptr, 0, 256);

    /* Below the 16 KiB split, so it must have come out of internal RAM. */
    TEST_ASSERT_LESS_THAN_UINT32(before,
                                 heap_caps_get_free_size(INTERNAL_8BIT));
    heap_caps_free(ptr);

    TEST_ASSERT_EQUAL_UINT32(before, heap_caps_get_free_size(INTERNAL_8BIT));
#else
    TEST_PASS_MESSAGE("CONFIG_ZLIB_PREFER_PSRAM is off; nothing to place");
#endif
}

TEST_CASE("a large zlib request prefers PSRAM", "[zlib][alloc]")
{
#if CONFIG_ZLIB_PREFER_PSRAM
    if (!psram_usable()) {
        TEST_PASS_MESSAGE("PSRAM is not initialized on this target");
        return;
    }
    const size_t psram_before = heap_caps_get_free_size(PSRAM_8BIT);
    const size_t internal_before = heap_caps_get_free_size(INTERNAL_8BIT);

    void *ptr = heap_caps_malloc_prefer(COMPONENT_PSRAM_THRESHOLD, 2,
                                        PSRAM_8BIT, INTERNAL_8BIT);
    TEST_ASSERT_NOT_NULL(ptr);
    memset(ptr, 0, COMPONENT_PSRAM_THRESHOLD);

    /* At or above the 16 KiB split, so a usable PSRAM must have served it. The
     * preference is not a guarantee: a full or fragmented PSRAM falls back to
     * internal RAM, which the second half of this check allows for. */
    const size_t psram_after = heap_caps_get_free_size(PSRAM_8BIT);
    const size_t internal_after = heap_caps_get_free_size(INTERNAL_8BIT);
    TEST_ASSERT_TRUE(psram_after < psram_before || internal_after < internal_before);

    heap_caps_free(ptr);
    TEST_ASSERT_EQUAL_UINT32(psram_before, heap_caps_get_free_size(PSRAM_8BIT));
    TEST_ASSERT_EQUAL_UINT32(internal_before, heap_caps_get_free_size(INTERNAL_8BIT));
#else
    TEST_PASS_MESSAGE("CONFIG_ZLIB_PREFER_PSRAM is off; nothing to place");
#endif
}

TEST_CASE("deflateInit draws its buffers from PSRAM", "[zlib][alloc]")
{
#if CONFIG_ZLIB_PREFER_PSRAM
    if (!psram_usable()) {
        TEST_PASS_MESSAGE("PSRAM is not initialized on this target");
        return;
    }
    const size_t before = heap_caps_get_free_size(PSRAM_8BIT);

    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    TEST_ASSERT_EQUAL_INT(Z_OK, deflateInit(&stream, Z_DEFAULT_COMPRESSION));

    /* The deflate window, hash chains and pending buffer are 64 KiB each, all
     * above the 16 KiB split, so PSRAM must have given some of it up. */
    const size_t after = heap_caps_get_free_size(PSRAM_8BIT);
    printf("deflateInit: PSRAM free %u -> %u bytes\n",
           (unsigned)before, (unsigned)after);
    TEST_ASSERT_LESS_THAN_UINT32(before, after);

    TEST_ASSERT_EQUAL_INT(Z_OK, deflateEnd(&stream));

    /* deflateEnd() must hand it all back, so the test leaks nothing. */
    TEST_ASSERT_EQUAL_UINT32(before, heap_caps_get_free_size(PSRAM_8BIT));
#else
    TEST_PASS_MESSAGE("CONFIG_ZLIB_PREFER_PSRAM is off; nothing to place");
#endif
}

TEST_CASE("inflate allocates its window from PSRAM", "[zlib][alloc]")
{
#if CONFIG_ZLIB_PREFER_PSRAM
    if (!psram_usable()) {
        TEST_PASS_MESSAGE("PSRAM is not initialized on this target");
        return;
    }
    /* inflate() allocates its window lazily, from updatewindow(), and only
     * once the output has wrapped. Decompressing into one large buffer
     * therefore never allocates it at all, so feed a small chunk at a time to
     * reach the case a streaming caller actually hits. */
    enum { SRC_SIZE = 32 * 1024, CHUNK = 512 };
    uint8_t *src = malloc(SRC_SIZE);
    TEST_ASSERT_NOT_NULL(src);
    fill_deterministic(src, SRC_SIZE);

    uLongf capacity = compressBound(SRC_SIZE);
    uint8_t *packed = malloc(capacity);
    TEST_ASSERT_NOT_NULL(packed);
    TEST_ASSERT_EQUAL_INT(Z_OK, compress2(packed, &capacity, src, SRC_SIZE,
                                          Z_DEFAULT_COMPRESSION));

    uint8_t *restored = malloc(SRC_SIZE);
    TEST_ASSERT_NOT_NULL(restored);

    const size_t before = heap_caps_get_free_size(PSRAM_8BIT);

    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    TEST_ASSERT_EQUAL_INT(Z_OK, inflateInit(&stream));
    stream.next_in = packed;
    stream.avail_in = (uInt)capacity;
    stream.next_out = restored;
    stream.avail_out = CHUNK;

    size_t produced = 0;
    int rc = Z_OK;
    while (rc != Z_STREAM_END) {
        rc = inflate(&stream, Z_NO_FLUSH);
        produced = (size_t)(stream.next_out - restored);
        if ((rc != Z_OK) && (rc != Z_STREAM_END) && (rc != Z_BUF_ERROR)) {
            TEST_FAIL_MESSAGE("inflate() failed on a valid stream");
        }
        if (stream.avail_out == 0) {
            /* Stop once the whole payload is out; the final chunk may end
             * exactly on the last byte, which is not an overflow. */
            if (produced >= SRC_SIZE) {
                break;
            }
            stream.next_out = restored + produced;
            stream.avail_out = CHUNK;
        }
        if (rc == Z_BUF_ERROR) {
            break;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(SRC_SIZE, (uint32_t)produced);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src, restored, SRC_SIZE);

    /* The 32 KiB window is above the 16 KiB split, so PSRAM must have served
     * it. */
    const size_t after = heap_caps_get_free_size(PSRAM_8BIT);
    printf("inflate: PSRAM free %u -> %u bytes\n",
           (unsigned)before, (unsigned)after);
    TEST_ASSERT_LESS_THAN_UINT32(before, after);

    TEST_ASSERT_EQUAL_INT(Z_OK, inflateEnd(&stream));

    /* inflateEnd() must release the window, so the test leaks nothing. */
    TEST_ASSERT_EQUAL_UINT32(before, heap_caps_get_free_size(PSRAM_8BIT));

    free(restored);
    free(packed);
    free(src);
#else
    TEST_PASS_MESSAGE("CONFIG_ZLIB_PREFER_PSRAM is off; nothing to place");
#endif
}

TEST_CASE("an application allocator is not overridden", "[zlib][alloc]")
{
    custom_alloc_calls = 0;
    custom_free_calls = 0;

    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    stream.zalloc = custom_zalloc;
    stream.zfree = custom_zfree;
    TEST_ASSERT_EQUAL_INT(Z_OK, deflateInit(&stream, Z_DEFAULT_COMPRESSION));

    /* deflateInit() must not have fallen back to the library default. */
    TEST_ASSERT_GREATER_THAN_UINT32(0, custom_alloc_calls);

    TEST_ASSERT_EQUAL_INT(Z_OK, deflateEnd(&stream));

    /* Every block the custom allocator handed out must come back through it,
     * otherwise freeing would go to heap_caps_free() on a plain malloc()ed
     * pointer. */
    TEST_ASSERT_EQUAL_UINT32(custom_alloc_calls, custom_free_calls);
}

TEST_CASE("a round trip works through the PSRAM allocator", "[zlib][alloc]")
{
    enum { SRC_SIZE = 48 * 1024 };
    uint8_t *src = malloc(SRC_SIZE);
    TEST_ASSERT_NOT_NULL(src);
    fill_deterministic(src, SRC_SIZE);

    uLongf capacity = compressBound(SRC_SIZE);
    uint8_t *packed = malloc(capacity);
    TEST_ASSERT_NOT_NULL(packed);
    TEST_ASSERT_EQUAL_INT(Z_OK, compress2(packed, &capacity, src, SRC_SIZE,
                                          Z_BEST_COMPRESSION));

    uint8_t *restored = malloc(SRC_SIZE);
    TEST_ASSERT_NOT_NULL(restored);
    uLongf restored_len = SRC_SIZE;
    TEST_ASSERT_EQUAL_INT(Z_OK, uncompress(restored, &restored_len,
                                           packed, capacity));
    TEST_ASSERT_EQUAL_UINT32(SRC_SIZE, (uint32_t)restored_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src, restored, SRC_SIZE);

    free(restored);
    free(packed);
    free(src);
}
