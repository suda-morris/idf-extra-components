/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "zlib.h"

/* Deterministic pseudo payload: a log-like text blob, good compression
 * material and big enough to show the effect. */
#define PAYLOAD_SIZE (16 * 1024)
/* deflate of a text payload never needs more than the input plus a few
 * header bytes; give zlib some slack plus room for the checksum trailers. */
#define COMPRESSED_CAP (PAYLOAD_SIZE + 64)

/* 64-byte lines, mostly repetitive text like a device log */
static void fill_payload(uint8_t *payload)
{
    for (int i = 0; i < PAYLOAD_SIZE; i++) {
        int col = i % 64;
        if (col < 10) {
            payload[i] = '0' + col;
        } else if (i / 64 % 8 < 4) {
            payload[i] = 'a' + (i % 26);
        } else {
            payload[i] = ' ';
        }
    }
}

void app_main(void)
{
    uint8_t *payload = malloc(PAYLOAD_SIZE);
    uint8_t *compressed = malloc(COMPRESSED_CAP);
    uint8_t *restored = malloc(PAYLOAD_SIZE);
    /* The example only shows how to call zlib, so a failed allocation is
     * fatal here instead of being handled. */
    ESP_ERROR_CHECK(payload == NULL || compressed == NULL || restored == NULL
                    ? ESP_ERR_NO_MEM : ESP_OK);

    fill_payload(payload);
    uLong payload_adler = adler32(adler32(0L, Z_NULL, 0), payload, PAYLOAD_SIZE);
    printf("payload: %u bytes, adler32 0x%08lx\n",
           (unsigned)PAYLOAD_SIZE, (unsigned long)payload_adler);

    /* One-shot compression: zlib's compress2() wraps deflate() with a zlib
     * stream header, Adler-32 checksum and trailer. Level 6 is the default
     * compromise between size and CPU time on embedded targets. */
    uLongf compressed_len = COMPRESSED_CAP;
    int err = compress2(compressed, &compressed_len,
                        payload, PAYLOAD_SIZE, Z_DEFAULT_COMPRESSION);
    ESP_ERROR_CHECK(err != Z_OK ? ESP_FAIL : ESP_OK);
    printf("compressed: %u -> %lu bytes (ratio %.1f%%)\n",
           (unsigned)PAYLOAD_SIZE, (unsigned long)compressed_len,
           100.0 * compressed_len / PAYLOAD_SIZE);

    /* One-shot decompression: the size of the original data must be known
     * up front; use the streaming inflate() API when it is not. */
    uLongf restored_len = PAYLOAD_SIZE;
    err = uncompress(restored, &restored_len, compressed, compressed_len);
    ESP_ERROR_CHECK(err != Z_OK ? ESP_FAIL : ESP_OK);
    printf("decompressed: %lu bytes\n", (unsigned long)restored_len);

    /* This is where the round trip is actually proven: same length, same
     * Adler-32 and a byte-for-byte memcmp() against the original payload.
     * Both checks are mandatory, and the failure paths abort so the result
     * can never be misread as success. */
    uLong restored_adler = adler32(adler32(0L, Z_NULL, 0), restored, restored_len);
    ESP_ERROR_CHECK(restored_len != PAYLOAD_SIZE ? ESP_FAIL : ESP_OK);
    printf("adler32: payload 0x%08lx == restored 0x%08lx\n",
           (unsigned long)payload_adler, (unsigned long)restored_adler);
    ESP_ERROR_CHECK(restored_adler != payload_adler ? ESP_FAIL : ESP_OK);
    ESP_ERROR_CHECK(memcmp(payload, restored, PAYLOAD_SIZE) != 0 ? ESP_FAIL : ESP_OK);
    printf("round trip verified: %u bytes match the original payload\n",
           (unsigned)PAYLOAD_SIZE);

    /* Buffers are ~32 KB in total, free them so the example does not leak
     * when app_main() is extended by a longer running application. */
    free(restored);
    free(compressed);
    free(payload);
}
