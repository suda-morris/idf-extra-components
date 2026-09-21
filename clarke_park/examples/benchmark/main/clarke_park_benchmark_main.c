/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

/*
 * Cost benchmark for the clarke_park component.
 *
 * It measures the three Park transform flavours that exist:
 *
 *   1. float                                    (clarke_park_park_f)
 *   2. Q15 fixed point, IQmath software sin/cos (clarke_park_park_iq15)
 *   3. Q15 fixed point, CORDIC hardware sin/cos (clarke_park_park_iq15, after
 *      clarke_park_enable_cordic())
 *
 * plus the matching inverse transforms and the four Clarke transforms.
 *
 * The point is the comparison, so all of them are printed side by side, and the
 * two Q15 runs (software and CORDIC) come from the same binary: the switch is a
 * a clarke_park_enable_cordic() or clarke_park_disable_cordic() call between the
 * two runs, so the compiler, the clock and the input data are identical and the only variable is
 * which sin/cos the transform reaches for.
 *
 * Every transform is called BENCH_ITERATIONS times with a varying input vector
 * (and, for the Park transforms, a varying angle), and the average cost per call
 * is reported. Timing is a CPU cycle count over the whole loop. The functions
 * under test, their callees (IQmath, sinf/cosf, the CORDIC driver) and this
 * example itself are placed in internal RAM by linker.lf, so the numbers are
 * not inflated by flash cache misses. (The float Park transform still calls
 * sinf/cosf in the C library, which remain in flash.)
 *
 * This is still a first order comparison tool: interrupts stay enabled and only
 * the average is reported.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_cpu.h"
#include "clarke_park.h"

/* Number of measured transform calls. Each call uses a different input vector
 * and (for the Park transforms) a different angle, so nothing can be hoisted
 * out of the loop and no call can be optimized away. */
#define BENCH_ITERATIONS 10000

/* Frequency of a typical motor control loop (16 kHz PWM). The cost of a
 * transform is also reported as a percentage of it, which is what usually
 * matters in practice. */
#define BENCH_LOOP_FREQ_HZ 16000

/* The Q15 angle sweep. Q15 covers [-1, 1), so [0, 1) rad is the widest range
 * every backend can represent. 940 steps of ~1.06 mrad keep the angle inside
 * the CORDIC input range with room to spare. */
#define BENCH_ANGLE_STEPS 940
#define BENCH_ANGLE_STEP  (1.0f / (float)BENCH_ANGLE_STEPS)

/* Type of a measured transform: the four loops of one backend share it */
typedef void (*bench_fn_t)(int sign);

/* ---------------------------------------------------------------- utilities */

/**
 * @brief CPU frequency in MHz, 0 if it is unknown for this target
 *
 * The value is the configured frequency (CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ). It
 * is only used to convert cycles into a time and into a load percentage, so it
 * does not affect the cycle counts themselves. It is wrong if the application
 * changes the CPU frequency at runtime (dynamic frequency scaling).
 */
static uint32_t bench_cpu_freq_mhz(void)
{
#ifdef CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ
    return (uint32_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
#else
    /* e.g. the Linux target has no meaningful CPU frequency, report cycles only */
    return 0;
#endif
}

static bool bench_cordic_built(void)
{
#if defined(CONFIG_SOC_CORDIC_SUPPORTED)
    return true;
#else
    return false;
#endif
}

/* ------------------------------------------------------------- measurement */

/*
 * Cycle count of one block that calls a transform BENCH_ITERATIONS times.
 * The call graph lives in internal RAM (see linker.lf), so the loop is not
 * paying for flash cache misses; running it once untimed before the
 * measurement only warms the branch predictor and the CORDIC pipeline.
 */

static void bench_report(const char *name, uint32_t cycles);

/**
 * @brief Run one measured transform block and report its average cost per call
 *
 * @param[in] name Name to print for the transform
 * @param[in] fn   The timed block: one transform run BENCH_ITERATIONS times
 */
static void bench_run(const char *name, void (*fn)(void))
{
    if (fn == NULL) {
        /* The transform is not available in this configuration */
        printf("%-30s not built\n", name);
        return;
    }

    fn();

    esp_cpu_cycle_count_t start = esp_cpu_get_cycle_count();
    fn();
    esp_cpu_cycle_count_t end = esp_cpu_get_cycle_count();

    bench_report(name, (uint32_t)(end - start));
}

/**
 * @brief Print one transform result
 *
 * @param[in] name   Name of the transform
 * @param[in] cycles Measured CPU cycles for BENCH_ITERATIONS calls
 */
static void bench_report(const char *name, uint32_t cycles)
{
    uint32_t cpu_freq_mhz = bench_cpu_freq_mhz();
    /* Average cycles per call, rounded to the nearest integer. */
    uint32_t cycles_per_call = (cycles + BENCH_ITERATIONS / 2) / BENCH_ITERATIONS;

    printf("%-30s", name);
    if (cpu_freq_mhz == 0) {
        printf("   %6" PRIu32 " cycles/call\n", cycles_per_call);
        return;
    }

    uint32_t ns_per_call = (uint32_t)(((uint64_t)cycles_per_call * 1000 + cpu_freq_mhz / 2) / cpu_freq_mhz);
    /* Cycles available per period of the control loop. */
    uint32_t cycles_per_loop = cpu_freq_mhz * (1000000 / BENCH_LOOP_FREQ_HZ);
    uint32_t load_permille = cycles_per_call * 1000 / cycles_per_loop;

    printf("   %6" PRIu32 " cycles  %6" PRIu32 " ns  %3" PRIu32 ".%01" PRIu32 " %%\n",
           cycles_per_call, ns_per_call, load_permille / 10, load_permille % 10);
}

/* ------------------------------------------------------------ float backend */

static void bench_float_clarke(void)
{
    clarke_park_uvw_f_t uvw = { .u = 0.8f, .v = -0.3f, .w = -0.5f };
    clarke_park_ab_f_t ab;

    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        uvw.u += 1e-6f;
        clarke_park_clarke(&uvw, &ab);
    }
}

static void bench_float_iclarke(void)
{
    clarke_park_uvw_f_t uvw;
    clarke_park_ab_f_t ab = { .alpha = 0.6f, .beta = 0.4f };

    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        ab.alpha += 1e-6f;
        clarke_park_iclarke(&ab, &uvw);
    }
}

static void bench_float_park(void)
{
    clarke_park_ab_f_t ab = { .alpha = 0.6f, .beta = 0.4f };
    clarke_park_dq_f_t dq;

    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        clarke_park_park((float)(i % BENCH_ANGLE_STEPS) * BENCH_ANGLE_STEP, &ab, &dq);
    }
}

static void bench_float_ipark(void)
{
    clarke_park_dq_f_t dq = { .d = 0.5f, .q = 0.2f };
    clarke_park_ab_f_t ab;

    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        clarke_park_ipark((float)(i % BENCH_ANGLE_STEPS) * BENCH_ANGLE_STEP, &dq, &ab);
    }
}

static void bench_float(void)
{
    printf("--- float backend ---\n");
    bench_run("clarke_f", bench_float_clarke);
    bench_run("iclarke_f", bench_float_iclarke);
    bench_run("park_f", bench_float_park);
    bench_run("ipark_f", bench_float_ipark);
}

/* --------------------------------------------------------------- Q15 backend
 *
 * The four transforms are written once and measured under both sin/cos sources:
 * the calls are the same, and which implementation the transform reaches for is
 * decided at run time by clarke_park_enable_cordic() and clarke_park_disable_cordic().
 */

static void bench_iq15_clarke(void)
{
    clarke_park_uvw_iq15_t uvw = { .u = _IQ15(0.8f), .v = _IQ15(-0.3f), .w = _IQ15(-0.5f) };
    clarke_park_ab_iq15_t ab;

    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        uvw.u += 1; /* smallest possible increment of the _iq15 type */
        clarke_park_clarke_iq15(&uvw, &ab);
    }
}

static void bench_iq15_iclarke(void)
{
    clarke_park_uvw_iq15_t uvw;
    clarke_park_ab_iq15_t ab = { .alpha = _IQ15(0.6f), .beta = _IQ15(0.4f) };

    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        ab.alpha += 1;
        clarke_park_iclarke_iq15(&ab, &uvw);
    }
}

static void bench_iq15_park(void)
{
    clarke_park_ab_iq15_t ab = { .alpha = _IQ15(0.6f), .beta = _IQ15(0.4f) };
    clarke_park_dq_iq15_t dq;

    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        clarke_park_park_iq15(_IQ15((float)(i % BENCH_ANGLE_STEPS) * BENCH_ANGLE_STEP), &ab, &dq);
    }
}

static void bench_iq15_ipark(void)
{
    clarke_park_dq_iq15_t dq = { .d = _IQ15(0.5f), .q = _IQ15(0.2f) };
    clarke_park_ab_iq15_t ab;

    for (int i = 0; i < BENCH_ITERATIONS; i++) {
        clarke_park_ipark_iq15(_IQ15((float)(i % BENCH_ANGLE_STEPS) * BENCH_ANGLE_STEP), &dq, &ab);
    }
}

static void bench_iq15(const char *source_name)
{
    printf("--- Q15 backend (%s) ---\n", source_name);
    bench_run("clarke_iq15", bench_iq15_clarke);
    bench_run("iclarke_iq15", bench_iq15_iclarke);
    bench_run("park_iq15", bench_iq15_park);
    bench_run("ipark_iq15", bench_iq15_ipark);
}

/* ------------------------------------------------------------------- config */

static void bench_print_config(void)
{
    uint32_t cpu_freq_mhz = bench_cpu_freq_mhz();

    printf("\n");
    printf("=== clarke_park benchmark ===\n");
    printf("Iterations     : %d\n", BENCH_ITERATIONS);
    printf("Optimization   : %s\n",
#if CONFIG_COMPILER_OPTIMIZATION_PERF
           "-O2 (COMPILER_OPTIMIZATION_PERF)"
#elif CONFIG_COMPILER_OPTIMIZATION_SIZE
           "-Os (COMPILER_OPTIMIZATION_SIZE)"
#elif CONFIG_COMPILER_OPTIMIZATION_DEBUG
           "-Og (COMPILER_OPTIMIZATION_DEBUG)"
#elif CONFIG_COMPILER_OPTIMIZATION_NONE
           "-O0 (COMPILER_OPTIMIZATION_NONE)"
#else
           "unknown, set COMPILER_OPTIMIZATION_PERF for comparable numbers"
#endif
          );
    printf("CORDIC built   : %s\n", bench_cordic_built() ? "yes" : "no");
    printf("Build target   : %s\n", CONFIG_IDF_TARGET);
    printf("CPU frequency  : ");
    if (cpu_freq_mhz != 0) {
        printf("%" PRIu32 " MHz\n", cpu_freq_mhz);
    } else {
        printf("unknown, times reported in cycles only\n");
    }
    printf("Timer          : CPU cycle counter (timed path in IRAM)\n");
    printf("\n");
}

static void bench_print_legend(void)
{
    printf("Cost per call, average over %d calls.\n", BENCH_ITERATIONS);
    if (bench_cpu_freq_mhz() != 0) {
        printf("The last column is the fraction of a %d Hz control loop period.\n", BENCH_LOOP_FREQ_HZ);
    }
    printf("\n");
}

/* ----------------------------------------------------------------- app_main */

/*
 * The Q15 backend is measured twice from the same binary: once on the IQmath
 * software tables, once on the CORDIC hardware (on a chip that has it). Same
 * compiler, same clock, same input - the only thing that changes is which sin/cos
 * the transform reaches for, which is exactly the comparison this example is for.
 */
static void bench_iq15_with_sources(void)
{
    /* Software first: this is what an application gets without asking for
     * anything, and it is the reference the hardware numbers are read against. */
    clarke_park_disable_cordic();
    bench_iq15("Q15 Park transform uses the IQmath software tables");
    printf("\n");

    esp_err_t ret = clarke_park_enable_cordic();
    if (ret != ESP_OK) {
        printf("--- Q15 backend: CORDIC unavailable (%s), nothing to compare ---\n",
               esp_err_to_name(ret));
        return;
    }

    bench_iq15("Q15 Park transform uses the CORDIC hardware");
    printf("\n");
}

void app_main(void)
{
    bench_print_config();
    bench_print_legend();

    bench_float();
    printf("\n");
    bench_iq15_with_sources();
    printf("\n");

    printf("clarke_park benchmark finished\n");
}
