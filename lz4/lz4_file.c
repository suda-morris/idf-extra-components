/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include "lz4_memory.h"

/*
 * lz4file.c allocates its FILE* staging buffers with malloc/calloc/free
 * directly. Redirect those calls to the component allocator so they follow
 * the same PSRAM-first policy as Block, HC, and Frame.
 */
#undef malloc
#undef calloc
#undef free
#define malloc LZ4_malloc
#define calloc LZ4_calloc
#define free LZ4_free

#include "lz4/lib/lz4file.c"
