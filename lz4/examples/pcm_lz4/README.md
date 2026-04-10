# LZ4 PCM Example

This example compresses and restores a raw, PCM-like audio buffer with LZ4 in an ESP-IDF application.

It generates a deterministic `int16_t` sample buffer in memory, so it does not need an audio input device or an input file. It then shows two patterns that are typical on ESP targets:

1. The Block API compresses and restores the complete buffer. The compression state is created once and reused (`LZ4_compress_fast_extState()`), which is the recommended approach for repeated Block compression.
2. The Frame API writes a self-describing frame and restores it in small chunks. The frame uses independent 64 KiB blocks so scratch RAM stays modest without PSRAM.

API contracts, other Frame options, and streaming details are in the upstream [LZ4 documentation](https://github.com/lz4/lz4/tree/dev/doc).

## Hardware Required

Any ESP32-series development board. No external hardware is required.

## Building and running

Run the application as usual for an ESP-IDF project. For example, for ESP32:

```bash
idf.py set-target esp32
idf.py build flash monitor -p PORT
```

Replace `PORT` with the serial port of your development board.

## Example output

The example should output lines similar to the following. Compressed sizes may vary if the input data or LZ4 preferences are changed:

```text
Block API example
source_size=8192 compressed_size=... verify=true

Frame streaming API example
source_size=8192 frame_size=... verify=true
```

The Block and Frame sizes are expected to be different.
A Frame carries format metadata and a content checksum, so it is slightly larger than a Block for the same input.

> [!NOTE]
> Compression is not guaranteed to make every input smaller.
> This example uses a repeatable, locally correlated signal so the effect is easy to see.
> Data that is already compressed (JPEG, H.264, MP3, and similar) may show little or no size reduction.
