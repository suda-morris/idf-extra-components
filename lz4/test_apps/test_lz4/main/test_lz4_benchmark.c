/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * This is an integration benchmark, not a replacement for the upstream LZ4
 * correctness or performance suite. It answers a narrower question: can the
 * LZ4 component run reliably on an ESP-IDF target, and what are the basic
 * Block API metrics on that target?
 *
 * The input is a deterministic, smooth periodic waveform with a slow offset
 * drift, so it represents a compressible raw sensor or audio buffer with the
 * long repeats that make LZ4 match-copy paths (including the fast decode
 * loop) observable in the numbers. It remains reproducible on every board.
 * Results should be compared only between the same target, CPU frequency,
 * optimization level, and memory configuration. They are not universal LZ4
 * performance claims.
 *
 * Note that the component builds the LZ4 library with LZ4_HEAPMODE=1, so
 * LZ4_compress_default() would allocate and free its hash table on every call.
 * This benchmark reuses an LZ4_stream_t through LZ4_compress_fast_extState()
 * (acceleration=1, same codec as the default helper) so the numbers reflect
 * the Block hot path rather than heap traffic.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "lz4.h"
#include "unity.h"

enum {
    /* 16-bit samples: 16 KiB of input data. */
    PCM_SAMPLE_COUNT = 8192,
    /* Enough repetitions to reduce timer granularity noise without making the
     * integration test unnecessarily long. */
    BENCH_ITERATIONS = 300,
};

typedef struct {
    /* Compressed bytes divided by original bytes; lower is better. */
    double ratio;
    /* Throughput for the Block compressor/decompressor; higher is better. */
    double block_comp_mbps;
    double block_decomp_mbps;
    /* One compression + decompression round trip; lower is better. */
    int64_t latency_us;
    /* Functional gate: 1 means the decompressed data matches the input. */
    int verify;
} bench_results_t;

static void print_bench_results(const bench_results_t *results)
{
    /* Keep these keys stable: CI and local scripts use them to compare runs. */
    printf("BENCH_RATIO=%.6f\n", results->ratio);
    printf("BENCH_BLOCK_COMP_MBPS=%.3f\n", results->block_comp_mbps);
    printf("BENCH_BLOCK_DECOMP_MBPS=%.3f\n", results->block_decomp_mbps);
    printf("BENCH_LATENCY_US=%lld\n", (long long)results->latency_us);
    printf("BENCH_VERIFY=%d\n", results->verify);
}

static void fill_pcm_like_samples(int16_t *samples, size_t sample_count)
{
    /*
     * Triangle wave with a slow offset drift: the waveform repeats exactly
     * every 192 samples, and the offset cycles every 32 periods, so the whole
     * pattern repeats every 6144 samples. This gives LZ4 abundant long
     * matches while keeping the signal smooth and fully deterministic. Do not
     * add per-sample random noise here: it would destroy the repeats, push
     * BENCH_RATIO towards 1.0, and hide match-copy performance differences.
     */
    enum { PERIOD = 192 };

    for (size_t i = 0; i < sample_count; i++) {
        const int32_t phase = (int32_t)(i % PERIOD);
        const int32_t triangle = (phase < PERIOD / 2) ? phase : (PERIOD - 1 - phase);
        const int32_t offset = ((int32_t)((i / PERIOD) % 32)) * 16;
        int32_t sample = (triangle - 48) * 256 + offset;

        if (sample > INT16_MAX) {
            sample = INT16_MAX;
        } else if (sample < INT16_MIN) {
            sample = INT16_MIN;
        }
        samples[i] = (int16_t)sample;
    }
}

static double elapsed_us_to_mbps(int64_t elapsed_us, size_t total_bytes)
{
    if (elapsed_us <= 0) {
        return 0.0;
    }

    const double seconds = (double)elapsed_us / 1000000.0;
    return ((double)total_bytes / seconds) / 1000000.0;
}

TEST_CASE("lz4 block benchmark reports verified metrics", "[lz4][benchmark]")
{
    bench_results_t results = {
        .ratio = -1.0,
        .block_comp_mbps = -1.0,
        .block_decomp_mbps = -1.0,
        .latency_us = -1,
        .verify = 0,
    };

    const size_t pcm_size = PCM_SAMPLE_COUNT * sizeof(int16_t);
    printf("LZ4 Block benchmark: %u bytes, %d iterations\n",
           (unsigned)pcm_size, BENCH_ITERATIONS);
    printf("Compare like-for-like builds: lower ratio/latency and higher throughput are better;\n");

    int16_t *pcm = malloc(pcm_size);
    if (pcm == NULL) {
        TEST_FAIL_MESSAGE("PCM allocation failed");
    }

    fill_pcm_like_samples(pcm, PCM_SAMPLE_COUNT);

    const int max_compressed_size = LZ4_compressBound((int)pcm_size);
    if (max_compressed_size <= 0) {
        free(pcm);
        TEST_FAIL_MESSAGE("LZ4_compressBound failed");
    }

    char *compressed = malloc((size_t)max_compressed_size);
    int16_t *restored = malloc(pcm_size);
    LZ4_stream_t *state = LZ4_createStream();
    if ((compressed == NULL) || (restored == NULL) || (state == NULL)) {
        LZ4_freeStream(state);
        free(restored);
        free(compressed);
        free(pcm);
        TEST_FAIL_MESSAGE("Benchmark buffer allocation failed");
    }

    const int compressed_size = LZ4_compress_fast_extState(state,
                                                           (const char *)pcm,
                                                           compressed,
                                                           (int)pcm_size,
                                                           max_compressed_size,
                                                           1);
    const int decoded_size = (compressed_size > 0)
                             ? LZ4_decompress_safe(compressed,
                                                   (char *)restored,
                                                   compressed_size,
                                                   (int)pcm_size)
                             : -1;

    const bool verified = (compressed_size > 0)
                          && (decoded_size == (int)pcm_size)
                          && (memcmp(pcm, restored, pcm_size) == 0);

    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        (void)LZ4_compress_fast_extState(state,
                                         (const char *)pcm,
                                         compressed,
                                         (int)pcm_size,
                                         max_compressed_size,
                                         1);
    }
    const int64_t comp_elapsed_us = esp_timer_get_time() - t0;

    int64_t decomp_elapsed_us = 0;
    if (compressed_size > 0) {
        t0 = esp_timer_get_time();
        for (int i = 0; i < BENCH_ITERATIONS; i++) {
            (void)LZ4_decompress_safe(compressed,
                                      (char *)restored,
                                      compressed_size,
                                      (int)pcm_size);
        }
        decomp_elapsed_us = esp_timer_get_time() - t0;
    }

    // Measure one complete Block round trip separately from the throughput loops.
    t0 = esp_timer_get_time();
    const int latency_comp_size = LZ4_compress_fast_extState(state,
                                                             (const char *)pcm,
                                                             compressed,
                                                             (int)pcm_size,
                                                             max_compressed_size,
                                                             1);
    const int latency_decoded_size = (latency_comp_size > 0)
                                     ? LZ4_decompress_safe(compressed,
                                                           (char *)restored,
                                                           latency_comp_size,
                                                           (int)pcm_size)
                                     : -1;
    const int64_t latency_us = esp_timer_get_time() - t0;

    const bool latency_verified = (latency_comp_size > 0)
                                  && (latency_decoded_size == (int)pcm_size)
                                  && (memcmp(pcm, restored, pcm_size) == 0);

    const bool final_verify = verified && latency_verified;

    results.ratio = (compressed_size > 0)
                    ? ((double)compressed_size / (double)pcm_size)
                    : -1.0;
    results.block_comp_mbps = elapsed_us_to_mbps(comp_elapsed_us,
                                                 pcm_size * BENCH_ITERATIONS);
    results.block_decomp_mbps = (compressed_size > 0)
                                ? elapsed_us_to_mbps(decomp_elapsed_us,
                                                     pcm_size * BENCH_ITERATIONS)
                                : -1.0;
    results.latency_us = latency_us;
    results.verify = final_verify ? 1 : 0;

    print_bench_results(&results);

    LZ4_freeStream(state);
    free(restored);
    free(compressed);
    free(pcm);

    TEST_ASSERT_EQUAL_INT(1, results.verify);
}
