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

/* The upstream stack relies on a single-core execution model: the periodic
   process runs from a timer ISR on upstream targets, so its unlocked
   datagram queue operations are mutually exclusive with the thread critical
   sections because those disable interrupts. On ESP-IDF the periodic process
   runs in a task, so pin every thread the OSAL creates (bus scan, non
   periodic) to one core together with the PDO cycle task
   (CONFIG_CHERRYECAT_CYCLE_TASK_CORE, default 0) to restore that
   assumption. */
#undef xTaskCreate
#define xTaskCreate(entry, name, stack_size, args, prio, handle) \
    xTaskCreatePinnedToCore((entry), (name), (stack_size), (args), (prio), (handle), CONFIG_CHERRYECAT_TASK_CORE)
