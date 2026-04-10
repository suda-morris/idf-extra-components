# Fast, lightweight LZ4 compression for ESP-IDF

[![Component Registry](https://components.espressif.com/components/espressif/lz4/badge.svg)](https://components.espressif.com/components/espressif/lz4)

This component packages the upstream [LZ4](https://github.com/lz4/lz4) lossless compression library for ESP-IDF.
It includes the Block, High Compression (HC), and Frame implementations, plus the `lz4file` helpers that read and write LZ4 Frames through standard `FILE*` streams.
The component does not add an ESP-specific wrapper, so data compressed by it can be exchanged with standard LZ4 implementations.

LZ4 is a good fit for buffers such as PCM samples, sensor data, and other raw data with local repetition.
It is normally not useful for data that is already compressed, such as JPEG, H.264, or MP3.

## Add the component

From an ESP-IDF project, add the registry dependency:

```bash
idf.py add-dependency "espressif/lz4^1.10.0"
```

Alternatively, add this to `main/idf_component.yml`:

```yaml
dependencies:
  espressif/lz4: "^1.10.0"
```

## Use it in a project

Include the upstream headers and call the upstream API:

```c
#include "lz4.h"       // Block API
#include "lz4hc.h"     // HC API (optional)
#include "lz4frame.h"  // Frame API (optional)
#include "lz4file.h"   // File helpers over FILE* streams (optional)
```

The [examples/pcm_lz4](examples/pcm_lz4) project shows a complete-buffer Block round trip and chunked Frame decompression on ESP-IDF.

For function contracts, streaming, format details, and security notes, use the official [LZ4 documentation](https://github.com/lz4/lz4/tree/dev/doc) and the headers.

> [!NOTE]
> The bundled `xxhash.c` is an internal dependency of the Frame implementation.
> Its API is not part of this component. Depend on a dedicated xxHash component
> if the application needs xxHash directly.

## On ESP-IDF

LZ4 uses opaque context objects to hold the state of compression and decompression operations. For memory planning, treat the context and its workspace as the memory allocated by LZ4 for the operation; these allocations come from the heap and do not consume task stack. Typical sizes are:

- Block hash table: 1-16 KiB (see Kconfig below)
- HC workspace: about 256 KiB
- Frame and `lz4file`: at least 64 KiB of contiguous RAM at the default 64 KiB block size; linked blocks need more

On chips without PSRAM, prefer the Block API, or Frame with independent 64 KiB blocks. HC and `lz4file` usually need PSRAM or a similarly large heap.

For repeated Block compression, create the state once and reuse it. `LZ4_compress_default()` allocates a hash table on every call:

```c
LZ4_stream_t *state = LZ4_createStream();
int n = LZ4_compress_fast_extState(state, src, dst, src_size, dst_capacity, 1);
LZ4_freeStream(state);
```

Do not place `LZ4_stream_t` or an HC stream on a small FreeRTOS task stack.

Every task must use its own context objects. Caller buffers need no extra locking unless they are shared.

## Kconfig

Available under **LZ4 Configurations**:

- `CONFIG_LZ4_USE_PSRAM` — on targets with PSRAM, large workspaces (HC, Frame scratch, `lz4file`) prefer PSRAM; Block hash tables stay in internal RAM. Either region is a fallback if the preferred one cannot satisfy the request.
- `CONFIG_LZ4_BLOCK_HASH_TABLE_SIZE_*` — Block hash table size (1-16 KiB, default 16 KiB). Smaller tables use less RAM and can be more cache-friendly, at the cost of compression ratio.
- `CONFIG_LZ4_FAST_DEC_LOOP` — faster decompression at about 3 KiB extra code size (about +17% on an ESP32-C5 with compressible input). Off by default.

## About License

The upstream LZ4 library sources included in this component come from the `lib` directory and are distributed under the BSD 2-Clause License.
This component does not use the other, non-BSD-licensed parts of the upstream LZ4 repository, such as the command-line tools and other repository content.

In addition to the upstream library, this component contains ESP-IDF porting and integration code.
That code is distributed under the Apache License 2.0.

The applicable license texts are included in [`LICENSE`](LICENSE).
