# Digital Filters for Signal Processing

[![Component Registry](https://components.espressif.com/components/espressif/filters/badge.svg)](https://components.espressif.com/components/espressif/filters)

## First-order IIR Filter

The transfer function of the first-order IIR filter is

$$
\frac{Y(z)}{X(z)} = \frac{b_0 + b_1z^{-1}}{1+a_1z^{-1}}
$$

thus

$$
y[n] = b_0 * x[n] + b_1 * x[n-1] - a_1 * y[n-1]
$$

You can use the Python `scipy.signal` module to design the filter coefficients.

```python
from scipy import signal

fs = 1000 # Sample frequency (Hz)
fc = 100  # Cutoff frequency (Hz)
b, a = signal.butter(1, fc, btype='lowpass', fs=fs)
```
