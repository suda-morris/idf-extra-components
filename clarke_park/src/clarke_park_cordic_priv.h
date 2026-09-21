/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "soc/soc_caps.h"
#include "esp_err.h"
#include "IQmathLib.h"

typedef void (*clarke_park_sincos_func_t)(_iq15 theta_rad, _iq15 *sin_theta, _iq15 *cos_theta);

#ifdef __cplusplus
extern "C" {
#endif

#if SOC_HAS(CORDIC)
#define CLARKE_PARK_WITH_CORDIC 1
#else
#define CLARKE_PARK_WITH_CORDIC 0
#endif

/**
 * @brief Q15 sin/cos through the CORDIC accelerator
 *
 * @param[in] theta_rad Angle in radians, Q15
 * @param[out] sin_theta Sine of the angle, Q15
 * @param[out] cos_theta Cosine of the angle, Q15
 */
void clarke_park_cordic_sincos_q15(_iq15 theta_rad, _iq15 *sin_theta, _iq15 *cos_theta);

/**
 * @brief Create the CORDIC engine, if it does not exist yet
 *
 * Called by clarke_park_enable_cordic(), so that the Park transform itself
 * never has to create or check anything.
 *
 * @return ESP_OK when the engine is ready, an error code otherwise
 */
esp_err_t clarke_park_cordic_engine_acquire(void);

/**
 * @brief Delete the CORDIC engine, if it exists
 *
 * Called by clarke_park_disable_cordic(), so the CORDIC unit is handed back to
 * the rest of the firmware
 * instead of staying reserved for a source that is no longer in use. Safe to
 * call when no engine was ever created.
 */
void clarke_park_cordic_engine_release(void);

#ifdef __cplusplus
} // extern "C"
#endif
