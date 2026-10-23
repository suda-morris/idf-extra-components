# zlib Example: Compress and Decompress Data

(See the README.md file in the upper level 'examples' directory for more information about examples.)

This example demonstrates the most common embedded use case of the zlib component: compressing an in-memory payload with the one-shot `compress2()` API, restoring it with `uncompress()`, and then proving that the round trip really returned the original bytes.

The application builds a 16 KB pseudo log payload, prints its Adler-32, compresses it at the default compression level and prints the compression ratio, decompresses it back, and finally compares the restored data against the original in three ways: the decompressed length, the Adler-32 of the restored buffer, and a byte-for-byte `memcmp()`. Every check is guarded by `ESP_ERROR_CHECK()`, so a mismatch aborts the run instead of being printed and ignored. No external hardware or network is required.

## How to Use Example

### Hardware Required

* An Espressif development board based on a chip listed in the supported targets table
* A USB cable for power supply and serial communication

### Set Chip Target

For example, to set esp32s3 as the chip target, run:

```
idf.py set-target esp32s3
```

### Build and Flash

Execute the following command to build the project, flash it to your development board, and run the monitor tool to view the serial output:

```
idf.py build flash monitor
```

(To exit the serial monitor, type ``Ctrl-]``.)

See the [Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/index.html) for full steps to configure and use ESP-IDF to build projects.

## Example Output

```
payload: 16384 bytes, adler32 0x26bd5781
compressed: 16384 -> 364 bytes (ratio 2.2%)
decompressed: 16384 bytes
adler32: payload 0x26bd5781 == restored 0x26bd5781
round trip verified: 16384 bytes match the original payload
```

The ratio varies only slightly between targets, because the payload is fixed in this example. The Adler-32 values are the real check on correctness: identical values before and after the round trip, plus a clean `memcmp()`, are the proof that compress/decompress works and not just that the APIs returned `Z_OK`.

## Example Breakdown

[`zlib_example_main.c`](main/zlib_example_main.c) is a straight top-to-bottom `app_main()`, no helper abstraction in between, and walks through the calls every embedded zlib user needs:

1. `compress2()` compresses a whole buffer in one call (`Z_DEFAULT_COMPRESSION` is level 6, a good size/CPU trade-off on microcontrollers).
2. `uncompress()` decompresses a whole buffer when the decompressed size is known in advance. For unknown or chunked sizes, switch to the streaming API: `inflateInit()` / `inflate()` / `inflateEnd()` (see the [zlib manual](https://www.zlib.net/manual.html) and [zlib How-To](https://www.zlib.net/zlib_how.html)).
3. `adler32()` computes zlib's checksum (the same one carried in the stream trailer); use `crc32()` when you need the 32-bit CRC instead. Here it is used as the round-trip check, which is the point of the example: `Z_OK` only means the calls did not fail, the checksum and `memcmp()` are what tell you the data survived intact.

The three buffers (payload, compressed stream and restored output, ~32 KB in total) are allocated from the heap with `malloc()` in `app_main()` and freed before it returns, so the example can coexist with other heap users; adjust `PAYLOAD_SIZE` and `COMPRESSED_CAP` for your own data, and allocate only what your application really needs.
