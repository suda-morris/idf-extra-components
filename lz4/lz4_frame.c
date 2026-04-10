/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <string.h>

#include "lz4_memory.h"

/*
 * lz4frame.c only honors LZ4_USER_MEMORY_FUNCTIONS when it is coalesced with
 * lz4.c (LZ4_SRC_INCLUDED). Provide the same ALLOC macros here so Frame
 * default-allocator paths — including the transient context inside
 * LZ4F_compressFrame() and LZ4F_createCDict() — go through LZ4_malloc
 * without --wrap or a new public API.
 */
#define LZ4_SRC_INCLUDED 1
#define MEM_INIT(p, v, s) memset((p), (v), (s))
#define ALLOC(s) LZ4_malloc(s)
#define ALLOC_AND_ZERO(s) LZ4_calloc(1, (s))
#define FREEMEM(p) LZ4_free(p)
#define KB *(1 << 10)
#define MB *(1 << 20)
#define GB *(1U << 30)

#include "lz4/lib/lz4frame.c"
