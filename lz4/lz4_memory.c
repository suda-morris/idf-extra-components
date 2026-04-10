/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdlib.h>

#include "sdkconfig.h"
#include "lz4_memory.h"

#if CONFIG_LZ4_USE_PSRAM
#include "esp_heap_caps.h"

/*
 * Block hash tables are 1-16 KiB and are walked with a random pattern, so
 * they stay in internal RAM. HC workspaces (~256 KiB), Frame scratch, and
 * lz4file staging buffers are at least 64 KiB and go to PSRAM first.
 */
#define LZ4_PSRAM_MIN_SIZE (32 * 1024)

static void *lz4_caps_malloc(size_t size)
{
    if (size >= LZ4_PSRAM_MIN_SIZE) {
        return heap_caps_malloc_prefer(size, 2,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return heap_caps_malloc_prefer(size, 2,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void *lz4_caps_calloc(size_t count, size_t size)
{
    size_t total = 0;

    if ((count > 0) && (size > (SIZE_MAX / count))) {
        return NULL;
    }
    total = count * size;
    if (total >= LZ4_PSRAM_MIN_SIZE) {
        return heap_caps_calloc_prefer(count, size, 2,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return heap_caps_calloc_prefer(count, size, 2,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
#endif /* CONFIG_LZ4_USE_PSRAM */

/*
 * LZ4_USER_MEMORY_FUNCTIONS replacements for malloc/calloc/free. Block, HC,
 * Frame (via lz4_frame.c), and lz4file (via lz4_file.c) all allocate through
 * these helpers.
 */
void *LZ4_malloc(size_t size)
{
#if CONFIG_LZ4_USE_PSRAM
    return lz4_caps_malloc(size);
#else
    return malloc(size);
#endif
}

void *LZ4_calloc(size_t count, size_t size)
{
#if CONFIG_LZ4_USE_PSRAM
    return lz4_caps_calloc(count, size);
#else
    return calloc(count, size);
#endif
}

void LZ4_free(void *ptr)
{
#if CONFIG_LZ4_USE_PSRAM
    heap_caps_free(ptr);
#else
    free(ptr);
#endif
}
