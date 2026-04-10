/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Upstream LZ4_USER_MEMORY_FUNCTIONS entry points. Not part of the
 * component's public API; applications should keep using malloc/free or
 * the LZ4F_*_advanced() custom-memory constructors.
 */
void *LZ4_malloc(size_t size);
void *LZ4_calloc(size_t count, size_t size);
void LZ4_free(void *ptr);

#ifdef __cplusplus
}
#endif
