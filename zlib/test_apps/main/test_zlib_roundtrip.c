/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "zlib.h"

#define SRC_SIZE (32 * 1024)

static void fill_deterministic(uint8_t *buf, size_t len)
{
    uint32_t state = 0xA5A5A5A5u;

    for (size_t i = 0; i < len; i++) {
        state = (state * 1664525u) + 1013904223u;
        buf[i] = (uint8_t)((state >> 24) ^ (uint32_t)i ^ ((uint32_t)i << 3));
    }
}

TEST_CASE("compress2 and uncompress round trip", "[zlib][compress]")
{
    uint8_t *src = malloc(SRC_SIZE);
    TEST_ASSERT_NOT_NULL(src);
    fill_deterministic(src, SRC_SIZE);

    uLongf capacity = compressBound(SRC_SIZE);
    uint8_t *packed = malloc(capacity);
    TEST_ASSERT_NOT_NULL(packed);

    TEST_ASSERT_EQUAL_INT(Z_OK, compress2(packed, &capacity, src, SRC_SIZE,
                                          Z_DEFAULT_COMPRESSION));
    /* The payload is pseudo-random, so deflate has to store most of it. What
     * matters is that the call succeeded and stayed inside the bound. */
    TEST_ASSERT_LESS_OR_EQUAL_UINT32((uint32_t)compressBound(SRC_SIZE),
                                     (uint32_t)capacity);

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

TEST_CASE("deflate and inflate stream round trip", "[zlib][compress]")
{
    uint8_t *src = malloc(SRC_SIZE);
    TEST_ASSERT_NOT_NULL(src);
    fill_deterministic(src, SRC_SIZE);

    z_stream deflate_stream;
    memset(&deflate_stream, 0, sizeof(deflate_stream));
    TEST_ASSERT_EQUAL_INT(Z_OK, deflateInit(&deflate_stream, Z_DEFAULT_COMPRESSION));

    uLongf capacity = deflateBound(&deflate_stream, SRC_SIZE);
    uint8_t *packed = malloc(capacity);
    TEST_ASSERT_NOT_NULL(packed);

    deflate_stream.next_in = src;
    deflate_stream.avail_in = SRC_SIZE;
    deflate_stream.next_out = packed;
    deflate_stream.avail_out = (uInt)capacity;
    TEST_ASSERT_EQUAL_INT(Z_STREAM_END, deflate(&deflate_stream, Z_FINISH));
    const uLong packed_len = capacity - deflate_stream.avail_out;
    TEST_ASSERT_EQUAL_INT(Z_OK, deflateEnd(&deflate_stream));

    z_stream inflate_stream;
    memset(&inflate_stream, 0, sizeof(inflate_stream));
    TEST_ASSERT_EQUAL_INT(Z_OK, inflateInit(&inflate_stream));

    uint8_t *restored = malloc(SRC_SIZE);
    TEST_ASSERT_NOT_NULL(restored);
    inflate_stream.next_in = packed;
    inflate_stream.avail_in = (uInt)packed_len;
    inflate_stream.next_out = restored;
    inflate_stream.avail_out = SRC_SIZE;
    TEST_ASSERT_EQUAL_INT(Z_STREAM_END, inflate(&inflate_stream, Z_FINISH));
    TEST_ASSERT_EQUAL_UINT32(0, inflate_stream.avail_out);

    TEST_ASSERT_EQUAL_UINT8_ARRAY(src, restored, SRC_SIZE);

    TEST_ASSERT_EQUAL_INT(Z_OK, inflateEnd(&inflate_stream));
    free(restored);
    free(packed);
    free(src);
}

TEST_CASE("deflateCopy carries the state over", "[zlib][compress]")
{
    uint8_t *src = malloc(SRC_SIZE);
    TEST_ASSERT_NOT_NULL(src);
    fill_deterministic(src, SRC_SIZE);

    z_stream base;
    memset(&base, 0, sizeof(base));
    TEST_ASSERT_EQUAL_INT(Z_OK, deflateInit(&base, Z_DEFAULT_COMPRESSION));

    /* Prime the stream so the copy is taken with history already in the
     * window, which is the case deflateCopy() exists for. The primed output is
     * kept: it is the first half of the final stream. */
    enum { PRIMED = 1024 };
    uint8_t *packed = malloc(SRC_SIZE * 2);
    TEST_ASSERT_NOT_NULL(packed);
    base.next_in = src;
    base.avail_in = SRC_SIZE / 2;
    base.next_out = packed;
    base.avail_out = PRIMED;
    TEST_ASSERT_EQUAL_INT(Z_OK, deflate(&base, Z_NO_FLUSH));
    const uLong primed_len = PRIMED - base.avail_out;

    z_stream copy;
    memset(&copy, 0, sizeof(copy));
    TEST_ASSERT_EQUAL_INT(Z_OK, deflateCopy(&copy, &base));

    /* The copy holds the primed history, so it only needs to compress the
     * second half of the payload. */
    copy.next_in = src + SRC_SIZE / 2;
    copy.avail_in = SRC_SIZE - SRC_SIZE / 2;
    copy.next_out = packed + primed_len;
    copy.avail_out = (SRC_SIZE * 2) - (uInt)primed_len;
    TEST_ASSERT_EQUAL_INT(Z_STREAM_END, deflate(&copy, Z_FINISH));
    const uLong packed_len = primed_len + ((SRC_SIZE * 2) - primed_len - copy.avail_out);

    /* Both streams stop mid-payload, so deflateEnd() reports Z_DATA_ERROR for
     * the unfinished base stream. What matters is that it frees everything. */
    TEST_ASSERT_EQUAL_INT(Z_OK, deflateEnd(&copy));
    TEST_ASSERT_EQUAL_INT(Z_DATA_ERROR, deflateEnd(&base));

    /* The stream the copy produced must decompress to the whole payload, which
     * is what proves the primed history really was carried over. */
    uint8_t *restored = malloc(SRC_SIZE);
    TEST_ASSERT_NOT_NULL(restored);
    z_stream inflate_stream;
    memset(&inflate_stream, 0, sizeof(inflate_stream));
    TEST_ASSERT_EQUAL_INT(Z_OK, inflateInit(&inflate_stream));
    inflate_stream.next_in = packed;
    inflate_stream.avail_in = (uInt)packed_len;
    inflate_stream.next_out = restored;
    inflate_stream.avail_out = SRC_SIZE;
    TEST_ASSERT_EQUAL_INT(Z_STREAM_END, inflate(&inflate_stream, Z_FINISH));
    TEST_ASSERT_EQUAL_UINT32(SRC_SIZE, SRC_SIZE - inflate_stream.avail_out);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(src, restored, SRC_SIZE);

    TEST_ASSERT_EQUAL_INT(Z_OK, inflateEnd(&inflate_stream));
    free(restored);
    free(packed);
    free(src);
}

TEST_CASE("a truncated stream is reported instead of overrunning", "[zlib][compress]")
{
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
    uLongf restored_len = SRC_SIZE;
    /* Cut the checksum trailer off: uncompress() must report the error. */
    TEST_ASSERT_NOT_EQUAL(Z_OK, uncompress(restored, &restored_len,
                                           packed, capacity - 2));

    free(restored);
    free(packed);
    free(src);
}
