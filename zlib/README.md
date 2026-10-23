# Zlib Compression Library

[![Component Registry](https://components.espressif.com/components/espressif/zlib/badge.svg)](https://components.espressif.com/components/espressif/zlib)

This ESP-IDF component packages [zlib](https://zlib.net/), the general-purpose lossless data-compression library. It provides the classic `deflate`/`inflate` APIs as well as the gzip-compatible streaming APIs (`gzopen`, `gzread`, ...), so you can compress and decompress data on ESP chips and interoperate with `.gz` files and other `deflate`-based formats (PNG, HTTP `Content-Encoding: deflate`, ZIP, and more).

The component builds the upstream sources through upstream's own CMake script, so the library stays identical to a stock zlib build of the same release.

## Memory Configuration

- `CONFIG_ZLIB_PREFER_PSRAM` (default on when `CONFIG_SPIRAM` is set) - the memory zlib allocates for itself comes from `heap_caps`: requests of 16 KiB or more prefer PSRAM, smaller ones prefer internal RAM, and either side falls back to the other. Turn it off to keep zlib on plain `malloc()` for speed.

The component's `zcalloc()`/`zcfree()` are part of the build either way; the option only selects the allocator they call, so switching it changes the memory map and nothing else.

Only zlib's own state and buffers are affected. The buffers an application passes to `compress2()`, `deflate()` or `inflate()` belong to the application and are allocated by the application. An allocator installed in `z_stream.zalloc`/`zfree` before `deflateInit()` or `inflateInit()` always takes precedence over the default, as zlib documents, and an application that supplies no allocator leaves the component's routing in place.

Without `CONFIG_ZLIB_PREFER_PSRAM`, zlib calls `malloc()`. With `CONFIG_SPIRAM_USE_CAPS_ALLOC`, `malloc()` never returns PSRAM; with `CONFIG_SPIRAM_USE_MALLOC`, only requests above `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` do. Enabling the option is therefore the only way to move zlib's working buffers out of internal RAM in either configuration.

## Using the Component

Add this component to your project with the [IDF Component Manager](https://docs.espressif.com/projects/idf-component-manager/en/latest/):

```yaml
dependencies:
  espressif/zlib: "^1.3.2"
```

or run `idf.py add-dependency "espressif/zlib^1.3.2"` in your project. Then include the header and link against the component:

```c
#include "zlib.h"
```

The component is named `zlib` in the build system, so any component that `PRIV_REQUIRES`/`REQUIRES` it (the component manager handles this automatically for managed components) gets the headers and the static library.

### Minimal Example

```c
#include "zlib.h"

/* Compress `src` into `dst`, returns the compressed size or 0 on error. */
size_t deflate_data(const uint8_t *src, size_t src_len,
                    uint8_t *dst, size_t dst_cap)
{
    uLongf dst_len = dst_cap;
    if (compress2(dst, &dst_len, src, src_len, Z_DEFAULT_COMPRESSION) != Z_OK) {
        return 0;
    }
    return dst_len;
}

/* Decompress `src` into `dst`, returns true on success. */
bool inflate_data(const uint8_t *src, size_t src_len,
                  uint8_t *dst, size_t *dst_len)
{
    uLongf len = *dst_len;
    if (uncompress(dst, &len, src, src_len) != Z_OK) {
        return false;
    }
    *dst_len = len;
    return true;
}
```

For streaming (chunked) compression and decompression, use `deflateInit()` / `deflate()` / `deflateEnd()` and `inflateInit()` / `inflate()` / `inflateEnd()`.

### Handling Large Payloads on Targets with Little Internal RAM

A stream's buffers are what dominate the memory cost of zlib, not the payload itself. At the default window and memLevel, `deflateInit()` takes roughly 260 KiB and `inflateInit()` roughly 40 KiB, most of it in blocks of 32 KiB and up. With `CONFIG_ZLIB_PREFER_PSRAM` enabled those blocks land in PSRAM, which is what makes zlib usable on a part with a few hundred KiB of internal RAM.

Do not place a `z_stream` on a small FreeRTOS task stack. The state structures are small, but the buffers behind them are on the heap, and the stack needs room for the call chain only.

## Programming Guide

The full API reference is the official zlib manual: https://www.zlib.net/manual.html

The usage examples under `examples/` in the upstream sources and [zlib.net's zlib How-To](https://www.zlib.net/zlib_how.html) cover streaming usage in depth. A runnable ESP-IDF example that compresses and decompresses a payload on the target lives in the [zlib_example](examples/zlib_example) directory of this component.
