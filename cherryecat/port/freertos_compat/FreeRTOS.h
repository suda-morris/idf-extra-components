/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Include shim for the vendored CherryECAT FreeRTOS OSAL, which includes the
   FreeRTOS headers without the "freertos/" path prefix that ESP-IDF exports.
   Redirecting here keeps the vendored OSAL source pristine. */
#pragma once
#include "freertos/FreeRTOS.h"

/* ESP-IDF renamed the ISR-context probe to portCHECK_IF_IN_ISR() (portmacro.h).
   Define the upstream OSAL's name here so ec_osal_freertos.c compiles unmodified. */
#ifndef xPortIsInsideInterrupt
#define xPortIsInsideInterrupt() ((BaseType_t)portCHECK_IF_IN_ISR())
#endif

/* The ESP-IDF FreeRTOS kernels take a spinlock argument in the task-level
   critical section macros; the vendored OSAL follows upstream FreeRTOS and
   calls them without arguments. The spinlock is unused by the IDF critical
   section implementation, so NULL keeps both kernel variants happy. */
#undef taskENTER_CRITICAL
#define taskENTER_CRITICAL() portENTER_CRITICAL(NULL)
#undef taskEXIT_CRITICAL
#define taskEXIT_CRITICAL() portEXIT_CRITICAL(NULL)
