/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdbool.h>
#include "clarke_park.h"
#include "clarke_park_cordic_priv.h"
#include "clarke_park_iq_priv.h"
#include "driver/cordic.h"
#include "esp_log.h"

/*
 * CORDIC-backed sin/cos for the Q15 Park transform.
 *
 * The implementation is deliberately minimal and free of synchronisation: it is
 * polling only (no interrupts), it takes no lock, and it keeps no state of its
 * own beyond the engine handle, which the application reserves by calling
 * clarke_park_enable_cordic(). See the programming guide for what that means
 * for concurrent users.
 */

/* The CORDIC precision field is 4 bits wide and the driver writes
 * `iteration_count - 1` into it, so the valid range is 1..15
 * (CORDIC_LL_PRECISION_MAX == 0xF). 15 iterations leave a residual of about
 * 2^-14, i.e. half a Q15 LSB, which is the best the hardware can do. */
#define CLARKE_PARK_CORDIC_ITERATIONS 15

/*
 * The angle reduction is a single multiply by 1/pi held in Q32:
 *
 *     round(2^32 / pi) = 1367130551 = 0x517CC1B7
 *
 * Two points decide this form, and both were checked on a 32-bit target
 * (riscv32imac, the same ISA class as esp32c3/esp32p4; Xtensa behaves the
 * same way for this pattern):
 *
 * 1. The precision has to come from the *constant*, not from a folding loop.
 *    A Q15 angle spans [-65536, 65536) rad, i.e. over 10430 turns of 2*pi, and
 *    the hardware argument only needs the position within one turn. Folding the
 *    angle turn by turn therefore needs a period known to about 2^-31 relative:
 *    a Q15 period (205887 instead of 205887.416) alone drifts by
 *    0.5 * 2^-15 * 10430 ~ 0.16 rad, which is 5000 LSB of Q15. The
 *    "subtract a multiple of 2*pi repeatedly" shape cannot meet that budget
 *    without carrying a wide period, and a wide period costs the very multiply
 *    it was meant to avoid.
 *
 * 2. That multiply is not expensive. The multiplier is a compile-time constant
 *    and only the 16 bits at [32, 48) of the product are consumed, so the
 *    compiler emits two 32x32 multiplies (mul/mulh on RISC-V, mull plus a
 *    high-word form on Xtensa) and about four ALU ops. Two multiplies is also
 *    the floor for any formulation, because the low 32 bits of theta*C do not
 *    determine the carry into bit 32. For reference, the IQmath reduction loop
 *    (_IQNsin_cos.c, the 14-step shift-and-compare at q=15) compiles to
 *    33 instructions plus a hardware division on the same target.
 *
 * The add of 2^31 is what makes the 16-bit truncation below a round-to-nearest
 * rather than a floor.
 */
#define CLARKE_PARK_CORDIC_INV_PI_Q32 0x517CC1B7u

/*
 * The CORDIC peripheral is a single hardware unit and the driver allows one
 * engine at a time, so the handle is kept here, process wide, and created on
 * request. There is no init and no deinit in the public API at all:
 * clarke_park_enable_cordic() and clarke_park_disable_cordic() are the lifecycle
 * knobs: enable acquires the unit and disable releases it.
 */
static cordic_engine_handle_t s_cordic_engine;

/**
 * @brief Q15 sin/cos through the CORDIC accelerator
 *
 * d = alpha * cos(theta) + beta * sin(theta) and friends need both values, so
 * the calculation is always made in pairs, both ways.
 */
void clarke_park_cordic_sincos_q15(_iq15 theta_rad, _iq15 *sin_theta, _iq15 *cos_theta)
{
    /* The CORDIC hardware (ESP_CORDIC_FUNC_COS) computes
     *     res1 = cos(pi * arg), res2 = sin(pi * arg)
     * with arg in [-1, 1) and both results in Q15.
     *
     * The angle therefore has to reach the peripheral as
     *
     *     arg = (theta / pi) mod 2
     *
     * theta is the Q15 encoding of the angle (theta_rad = theta_q15 / 2^15), so
     * theta/pi in Q15 units is simply theta_q15 / pi: the 2^15 factors cancel
     * and the constant does not depend on the caller's Q format.
     *
     * The whole reduction is the multiply plus the 16-bit truncation below:
     * theta * 1/pi lands on integers of pi, so keeping 16 bits keeps exactly one
     * period, and reading that back as a signed Q15 number maps it onto the
     * [-1, 1) window the peripheral wants. No branch and no loop are involved;
     * see the constant above for why that beats folding here. */
    int32_t arg_q15 = (int32_t)((((int64_t)theta_rad * (int64_t)CLARKE_PARK_CORDIC_INV_PI_Q32) +
                                 (int64_t)0x80000000) >> 32);
    uint32_t arg = (uint32_t)(uint16_t)(int16_t)arg_q15;
    uint32_t res1 = 0;
    uint32_t res2 = 0;

    if (s_cordic_engine != NULL) {
        cordic_calculate_config_t calc_cfg = {
            .function = ESP_CORDIC_FUNC_COS, /* res1 = cos(pi*arg), res2 = sin(pi*arg) */
            .iq_format = ESP_CORDIC_FORMAT_Q15,
            .iteration_count = CLARKE_PARK_CORDIC_ITERATIONS,
            .scale_exp = 0,
        };
        cordic_input_buffer_desc_t input = {
            .p_data_arg1 = &arg,
            .p_data_arg2 = NULL,
        };
        cordic_output_buffer_desc_t output = {
            .p_data_res1 = &res1,
            .p_data_res2 = &res2,
        };

        if (cordic_calculate_polling(s_cordic_engine, &calc_cfg, &input, &output, 1) == ESP_OK) {
            *cos_theta = (int16_t)(res1 & 0xFFFF);
            *sin_theta = (int16_t)(res2 & 0xFFFF);
            return;
        }
    }

    /* No engine (the unit belongs to somebody else) or the accelerator refused
     * the job: use the IQmath software tables so the transform still returns a
     * result. */
    *sin_theta = CLARKE_PARK_IQ_SIN(15, theta_rad);
    *cos_theta = CLARKE_PARK_IQ_COS(15, theta_rad);
}

esp_err_t clarke_park_cordic_engine_acquire(void)
{
    if (s_cordic_engine != NULL) {
        return ESP_OK;
    }

    cordic_engine_config_t engine_cfg = {
        .clock_source = CORDIC_CLK_SRC_DEFAULT,
    };
    return cordic_new_engine(&engine_cfg, &s_cordic_engine);
}

void clarke_park_cordic_engine_release(void)
{
    if (s_cordic_engine != NULL) {
        cordic_delete_engine(s_cordic_engine);
        s_cordic_engine = NULL;
    }
}
