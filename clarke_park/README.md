# Clarke Park Transform

[![Component Registry](https://components.espressif.com/components/espressif/clarke_park/badge.svg)](https://components.espressif.com/components/espressif/clarke_park)

Three phase currents that all wiggle? The Clarke and Park transforms turn them into two calm numbers your control loop can actually use. This component gives you both transforms (plus their inverses) in `float` and in `IQmath` fixed-point, behind a single API.

## What you get

- **Clarke / inverse Clarke** - three phases (U/V/W) to two stator axes (alpha/beta) and back.
- **Park / inverse Park** - stator axes to rotor axes (d/q) and back, so that a constant-speed machine gives you nearly constant values.
- **Two numeric backends in the same firmware** - `float` for convenience, IQmath fixed-point for speed. Pick the backend with the coordinate type you pass in; the calls do not change.
- **Every IQmath Q-format from `_iq8` to `_iq28` at the same time** - the format is picked from the types you pass in, so nothing has to be configured. Unused formats are dropped by the linker.
- **Optional CORDIC hardware acceleration for Q15** - on chips that have the peripheral, `clarke_park_enable_cordic()` moves the `sin`/`cos` of the Q15 Park transform into hardware. Off by default, opt-in at run time.
- **Nothing to configure, nothing to initialize** - the transforms are pure math, and no Kconfig option takes part in picking the backend: the coordinate type picks the format, and the application picks the sin/cos source.

## Add it to your project

```bash
idf.py add-dependency "espressif/clarke_park"
```

## Quick start

```c
#include "clarke_park.h"

void example(float theta_rad)
{
    clarke_park_uvw_f_t uvw = { .u = 1.0f, .v = -0.5f, .w = -0.5f };
    clarke_park_ab_f_t ab;
    clarke_park_dq_f_t dq;

    clarke_park_clarke(&uvw, &ab);         // U/V/W  -> alpha/beta
    clarke_park_park(theta_rad, &ab, &dq); // alpha/beta -> d/q

    // the fixed-point backend uses the very same calls
    clarke_park_uvw_iq15_t uvw_iq = { .u = _IQ15(1.0f), .v = _IQ15(-0.5f), .w = _IQ15(-0.5f) };
    clarke_park_ab_iq15_t ab_iq;
    clarke_park_dq_iq15_t dq_iq;

    clarke_park_clarke(&uvw_iq, &ab_iq);
    clarke_park_park(_IQ15(theta_rad), &ab_iq, &dq_iq);
}
```

`theta` is the electrical angle in radians: a `float` for the float backend, an `_iqN` value for the fixed-point backend (use the `_IQN()` helper that matches the type).

## IQmath Q-formats

The Q-format is selected by the **coordinate type**. Different formats can be used in one firmware, including in the same translation unit:

```c
clarke_park_uvw_iq15_t uvw15 = { .u = _IQ15(1.0f), .v = _IQ15(-0.5f), .w = _IQ15(-0.5f) };
clarke_park_ab_iq15_t ab15;
clarke_park_clarke(&uvw15, &ab15);          /* Q15 arithmetic */

clarke_park_uvw_iq8_t uvw8 = { .u = _IQ8(1.0f), .v = _IQ8(-0.5f), .w = _IQ8(-0.5f) };
clarke_park_ab_iq8_t ab8;
clarke_park_clarke(&uvw8, &ab8);            /* Q8 arithmetic */
```

The unsuffixed `_iq` API follows `GLOBAL_IQ` of the translation unit that includes `clarke_park.h`. The wrappers do not freeze the format at whatever `GLOBAL_IQ` happened to be when this component was compiled. `_iq1`..`_iq7` / `GLOBAL_IQ<8`, `_iq29` / `GLOBAL_IQ=29` and `_iq30` / `GLOBAL_IQ=30` are not supported.

> [!NOTE]
> IQmath `_iqN` scalars are all `int32_t`, so Park's angle must be converted with the matching `_IQN()` helper.

## Configuration

The component has no Kconfig options. The Q-format is a property of the
coordinate type you pass in, and whether to use the CORDIC hardware is a run time
decision the application makes, so there is no format to configure and no init
call to remember.

### CORDIC hardware acceleration

On a chip with the [CORDIC peripheral](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s31/api-reference/peripherals/cordic.html),
ask for the hardware at start-up:

```c
#include "clarke_park.h"

void app_main(void)
{
    /* Q15 Park transforms now use the CORDIC peripheral. */
    ESP_ERROR_CHECK(clarke_park_enable_cordic());
}
```

Without that call the Q15 Park transform keeps using the IQmath software tables,
which is the default.

- Only Q15 can be accelerated; the CORDIC hardware has no other format. Q8..Q14, Q16..Q28 keep using IQmath.
- The float backend is never affected.
- `clarke_park_enable_cordic()` and `clarke_park_disable_cordic()` are declared on every chip. Enabling on a target without the peripheral (`SOC_CORDIC_SUPPORTED`) returns `ESP_ERR_NOT_SUPPORTED` and the transform stays on IQmath. The CORDIC backend itself is compiled only on chips that have the peripheral.
- The CORDIC engine is created by `clarke_park_enable_cordic()` and released by `clarke_park_disable_cordic()`. If another driver already owns the unit, enabling fails and the transform stays on IQmath.
- The Q15 Park transforms take **no lock** and can be called from an ISR. Making that safe is the application's job - see the programming guide.
- The hardware changes the numerics slightly: the CORDIC residual is about half a Q15 LSB, below the Q15 quantization step, and the angle is reduced to `[-pi, pi)` in the backend, so a Q15 angle anywhere in the range of the type is accepted.

## Benchmark

`examples/benchmark` measures `float` and Q15 with and without the accelerator on the target, as the average cost per call (CPU cycles, timed path in IRAM):

```
idf.py -C examples/benchmark -B build build flash monitor -p (PORT)
```

The example only builds on `esp32s31`, the chip that has the peripheral, and
measures both the software and the hardware Q15 path from a single binary. See
[examples/benchmark/README.md](examples/benchmark/README.md) for how to read the
numbers.

## Where to go next

The full story - what the transforms do, the formulas, the Q-format trade-offs,
the CORDIC integration, thread and ISR safety, accuracy and benchmark numbers -
lives in the programming guide:

- **Programming Guide and API Reference**:
  [Clarke Park Transform Documentation](https://espressif.github.io/idf-extra-components/latest/clarke_park/index.html)

## Changelog

See [CHANGELOG.md](CHANGELOG.md).
