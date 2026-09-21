/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "clarke_park.h"
#include "clarke_park_private.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Q15 sin/cos using the currently selected backend
 */
void clarke_park_sincos_q15(_iq15 theta_rad, _iq15 *sin_theta, _iq15 *cos_theta);

#ifdef __cplusplus
} // extern "C"
#endif

/*
 * Backend helpers. Q is a decimal token pasted onto _IQN() / _IQNmpy /
 * _IQNsin / _IQNcos, so every call resolves to its own Q-format at compile
 * time. _IQdiv2 is the same shift for every Q.
 */

#define CLARKE_PARK_IQ_DIV2(_v) _IQdiv2(_v)

#define CLARKE_PARK_IQ_MPY(_q, _a, _b) CLARKE_PARK_IQ_MPY_I(_q, _a, _b)
#define CLARKE_PARK_IQ_MPY_I(_q, _a, _b) _IQ##_q##mpy(_a, _b)

/* Explicitly per-format sin/cos, for the fallback inside clarke_park_cordic.c.
 * The transforms themselves go through CLARKE_PARK_IQ_SINCOS in
 * clarke_park_iq.c, which can route Q15 to the CORDIC hardware. */
#define CLARKE_PARK_IQ_SIN(_q, _v) CLARKE_PARK_IQ_SIN_I(_q, _v)
#define CLARKE_PARK_IQ_SIN_I(_q, _v) _IQ##_q##sin(_v)

#define CLARKE_PARK_IQ_COS(_q, _v) CLARKE_PARK_IQ_COS_I(_q, _v)
#define CLARKE_PARK_IQ_COS_I(_q, _v) _IQ##_q##cos(_v)

#define CLARKE_PARK_IQ_FROM_FLOAT(_q, _v) CLARKE_PARK_IQ_FROM_FLOAT_I(_q, _v)
#define CLARKE_PARK_IQ_FROM_FLOAT_I(_q, _v) _IQ##_q(_v)
