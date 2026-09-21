# Clarke-Park benchmark

Measures the cost of every `clarke_park` transform in CPU cycles, on
**esp32s31**, the only chip with the CORDIC peripheral this example exists to
measure.

The timed path lives in internal RAM: `linker.lf` places this example, the
`clarke_park` transforms, IQmath and the CORDIC driver in IRAM (and their
lookup tables in DRAM). A bare `esp_cpu_get_cycle_count()` around the loop is
then a fair comparison - it is not inflated by flash cache misses. The float
Park transform still calls `sinf`/`cosf` in the C library, which stay in flash.

The three Park implementations are put next to each other:

| Backend | Line in the output | sin/cos implementation |
| --- | --- | --- |
| float | `park_f` | `sinf` / `cosf` from the C library |
| Q15 fixed point | `park_iq15` | IQmath software tables |
| Q15 fixed point | `park_iq15` | CORDIC hardware peripheral |

The two Q15 rows are the *same code and the same API*: what changes is the call
to `clarke_park_enable_cordic()` between the two measurement runs. Both come
from a single binary, so the compiler, the clock and the input data are identical
and the only variable left is which sin/cos the transform reaches for.

Each transform is called 10000 times with a varying input vector (and, for the
Park transforms, an angle sweeping `[0, 1)` rad) so that nothing can be hoisted
out of the loop. The result is printed as the average cycles per call, the
equivalent time at the configured CPU frequency and the load in a 16 kHz
control loop.

## Build and run

```
idf.py -C examples/benchmark -B build build flash monitor -p (PORT)
```

The example builds with `-O2` and a fixed CPU frequency (`sdkconfig.defaults`),
because the numbers are only comparable when the compiler and the clock are the
same. The CORDIC backend is compiled in because the target has the peripheral;
the application - here, the benchmark itself - decides when to take the hardware.
The banner it prints always states the configuration the numbers belong to.

On a chip without the peripheral the example still runs and prints the software
numbers plus a reminder that there was nothing to compare against.

## Reading the output

```
--- float backend ---
clarke_f                          8 cycles     33 ns    0.0 %
iclarke_f                         7 cycles     29 ns    0.0 %
park_f                          156 cycles    650 ns    1.0 %
ipark_f                         158 cycles    658 ns    1.0 %

--- Q15 backend (Q15 Park transform uses the IQmath software tables) ---
clarke_iq15                      11 cycles     45 ns    0.0 %
iclarke_iq15                      9 cycles     37 ns    0.0 %
park_iq15                       280 cycles   1166 ns    1.8 %
ipark_iq15                      278 cycles   1158 ns    1.7 %

--- Q15 backend (Q15 Park transform uses the CORDIC hardware) ---
clarke_iq15                      11 cycles     45 ns    0.0 %
iclarke_iq15                      9 cycles     37 ns    0.0 %
park_iq15                        42 cycles    175 ns    0.2 %
ipark_iq15                       44 cycles    183 ns    0.3 %
```

What the numbers usually show:

- The Clarke transforms are a handful of cycles: pure arithmetic, no
  trigonometry. They are identical in both Q15 runs, which is a useful sanity
  check that the CORDIC source only touches the trigonometry of the Park
  transforms.
- The Park transforms are dominated by the trigonometry, so they cost far more
  than the Clarke transforms on the software tables.
- The float backend pays for a full `sinf`/`cosf` pair; the Q15 CORDIC run
  replaces both with one hardware round trip.

The numbers above are an illustration - run the example on your board to get the
real ones.

## Caveats

- The CORDIC hardware computes in Q15, so it produces a slightly different
  result than the IQmath tables (about half a Q15 LSB, i.e. below the Q15
  quantization step). Compare costs, not bit patterns.
- If another driver already owns the CORDIC unit, `clarke_park_enable_cordic()`
  fails with `ESP_ERR_NOT_FOUND`, the benchmark says so and the Q15 numbers are
  the software ones. Run the benchmark on a bare board to see the hardware
  numbers.
- The float Park transform still calls `sinf`/`cosf` in the C library, which
  this example does not move to IRAM. Treat the float numbers as including
  that library cost.
- This is a first order comparison tool: it leaves interrupts enabled and only
  reports the average, so treat the numbers as indicative, not as cycle accurate
  measurements of a single call.
