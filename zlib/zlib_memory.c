/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "sdkconfig.h"

#if CONFIG_ZLIB_PREFER_PSRAM
#include "esp_heap_caps.h"

/*
 * Requests of this many bytes or more prefer PSRAM, smaller ones prefer
 * internal 8-bit RAM. Either side falls back to the other, so this only
 * decides which region is tried first.
 *
 * zlib's buffers are all well above it: the deflate window, the two hash
 * chains and the pending buffer are 64 KiB each at the default memLevel, and
 * the inflate window is 32 KiB. The state structures are the exception at
 * roughly 1 KiB (deflate) and 7 KiB (inflate), so they keep staying in
 * internal RAM. They are read and written on every symbol, while the buffers
 * are walked sequentially.
 */
#define ZLIB_PSRAM_THRESHOLD (16 * 1024)

static void *zlib_malloc(size_t size)
{
    if (size >= ZLIB_PSRAM_THRESHOLD) {
        return heap_caps_malloc_prefer(size, 2,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return heap_caps_malloc_prefer(size, 2,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void zlib_free(void *ptr)
{
    heap_caps_free(ptr);
}
#else
/*
 * Plain malloc() and free(). With CONFIG_SPIRAM_USE_CAPS_ALLOC malloc() never
 * returns PSRAM; with CONFIG_SPIRAM_USE_MALLOC only requests above
 * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL do.
 */
static void *zlib_malloc(size_t size)
{
    return malloc(size);
}

static void zlib_free(void *ptr)
{
    free(ptr);
}
#endif /* CONFIG_ZLIB_PREFER_PSRAM */

/*
 * Upstream's own hook for the stream allocator. CMakeLists.txt defines
 * MY_ZCALLOC for zlibstatic, which drops zlib's zcalloc()/zcfree() from
 * zutil.c and lets these definitions take their place, so no upstream source
 * file is modified. The hook is always active; which allocator sits behind it
 * is decided above, at compile time.
 *
 * Everything zlib allocates for a stream goes through the ZALLOC/ZFREE macros,
 * which call zalloc/zfree out of the z_stream: the deflate state, window,
 * hash chains and pending buffer, the inflate state and window, the
 * inflateBack window, and the copies made by deflateCopy() and
 * inflateCopy(). compress() and uncompress() clear those two fields, so the
 * deflateInit2_()/inflateInit2_() inside them fall back to zcalloc as well,
 * which puts compress2() and uncompress() on this path too.
 *
 * These functions are only what deflateInit2_() and inflateInit2_() install
 * for a stream whose zalloc/zfree were left NULL, so an application that
 * fills those fields in itself keeps its own allocator, as zlib documents.
 *
 * The signatures are copied from the declarations in zutil.h, which is
 * private to the library.
 */
void *zcalloc(void *opaque, unsigned items, unsigned size);
void zcfree(void *opaque, void *ptr);

void *zcalloc(void *opaque, unsigned items, unsigned size)
{
    (void)opaque;
    if ((items > 0) && (size > (SIZE_MAX / items))) {
        /* Upstream multiplies unchecked. Wrapping around would hand zlib a
         * short buffer to overrun rather than failing the allocation. */
        return NULL;
    }
    return zlib_malloc((size_t)items * size);
}

void zcfree(void *opaque, void *ptr)
{
    (void)opaque;
    zlib_free(ptr);
}
