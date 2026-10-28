# Raylib component

[![Component Registry](https://components.espressif.com/components/espressif/raylib/badge.svg)](https://components.espressif.com/components/espressif/raylib)

[Raylib](https://www.raylib.com/) is a small library for 2D and 3D graphics, built around a plain C API (`InitWindow()`, `BeginDrawing()`, `DrawText()`, `EndDrawing()`). This component packages it for ESP-IDF and runs it on raylib's **software renderer**, so it works on any ESP32 chip with a display, no GPU required.

The raylib API is unchanged. Everything below is about how frames reach your panel.

## What you get

- The upstream raylib 6.2 API, built from upstream's own CMake files
- Software rasteriser (rlsw): 2D shapes, textures, text, plus the parts of the 3D API that do not need a GPU
- A framebuffer format you pick: RGB565 for `esp_lcd` panels, RGBA8888 to skip the
  conversion, RGB332 for the smallest buffers
- Frames handed to your panel through one callback you write
- A present pipeline: the renderer draws into one of two double-buffered
  framebuffers while an internal task streams the previous frame to the panel,
  so the panel transfer and the rendering overlap
- Large buffers placed in PSRAM, small ones kept in internal RAM
- Flash cost you control: image loaders are a menuconfig choice

## Add it to your project

```bash
idf.py add-dependency "espressif/raylib^6.2.0"
```

Or list it in `main/idf_component.yml`:

```yaml
dependencies:
  espressif/raylib: "^6.2.0"
```

Then include the upstream header plus the small ESP-IDF helper:

```c
#include "raylib.h"
#include "esp_raylib.h"
```

## Wire up a display

You initialise your panel with `esp_lcd` as usual, then tell raylib where to send frames:

```c
static void panel_flush(esp_raylib_display_handle_t handle, const uint16_t *pixels,
                        uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    esp_lcd_panel_draw_bitmap((esp_lcd_panel_handle_t)handle, x, y, x + w, y + h, pixels);
}

...
const esp_raylib_display_config_t display = {
    .handle     = panel,
    .width      = 320,
    .height     = 240,
    .swap_bytes = true,    /* RGB565 out MSB first; zero would mean no swap */
    .invert     = true,    /* panel origin at the bottom */
    .chunk_rows = 48,      /* send 48 lines at a time, fits SPI DMA */
    .flush_cb   = panel_flush,
};
esp_raylib_set_display(&display);

InitWindow(320, 240, "hello");
```

From there it is plain raylib:

```c
while (!WindowShouldClose()) {
    BeginDrawing();
    ClearBackground(RAYWHITE);
    DrawText("Hello raylib!", 20, 100, 24, DARKGRAY);
    EndDrawing();
}
```

`esp_raylib_set_display()` must be called before `InitWindow()`, and the task that calls `InitWindow()` is the one that renders. Give it a generous stack, 64 KiB is a reasonable start, since the renderer keeps texture and rasteriser state there.

The `chunk_rows` split is what makes SPI panels practical: instead of one 150 KiB transfer, raylib hands you 48 lines at a time. Leave it at 0 to get whole frames in a single callback. `flush_cb` must consume the pixels before it returns — `esp_lcd_panel_draw_bitmap()` queues its DMA and returns early, so wait for its `on_color_trans_done` event or copy the band first.

## Configure

Run `idf.py menuconfig` and open **Component config → Raylib**:

| Option | Default | Effect |
| --- | --- | --- |
| `CONFIG_RAYLIB_FRAMEBUFFER_FORMAT` | RGB565 | Pixel format of the buffer the renderer draws into, and of the pixels your `flush_cb` receives. 16 bpp is what panels take natively; RGBA8888 is raylib's own format and needs no conversion; RGB332 is for 640x480 buffers without PSRAM. |
| `CONFIG_RAYLIB_USE_PSRAM` | on when PSRAM is available | Place large buffers in PSRAM instead of internal RAM. Blocks of 32 KiB and above go to PSRAM (the split point is fixed in [`esp_raylib_memory.c`](src/esp_raylib_memory.c)); turn it off and a 320x240 RGB565 framebuffer alone eats 150 KiB of internal RAM. |
| `CONFIG_RAYLIB_FILEFORMAT_*` | all off | Which resource loaders are linked in, independently combinable: QOI, PNG, BMP, GIF and DDS images, and TTF fonts. QOI is raylib's own format and the cheapest; PNG adds raylib's built-in decoder, no zlib needed; DDS only helps for uncompressed textures, the software renderer cannot sample GPU-compressed ones. With all off, assets have to come from memory, for example data embedded in the binary. |

Only those are user-facing. Everything else the build pins - disabled modules, rlgl limits, input queue sizes - is a short list of `-D` flags in [CMakeLists.txt](CMakeLists.txt). They go straight to the compiler, so raylib's own `src/config.h` keeps its defaults for everything the list does not mention.

## What this component changes

The upstream API is intact, so raylib tutorials and examples apply. What differs is the environment:

- **Software rendering only.** There is no hardware OpenGL on ESP32, so raylib's rlsw rasteriser draws into a system-memory buffer. 3D calls that rely on GPU features are not available; 2D graphics, textures and text are.
- **No window, no input.** There is no window manager, keyboard, mouse or clipboard, so those calls are present but inert. Feed input yourself if your board has a touch panel or buttons.
- **Disabled modules.** `rmodels` (3D model loading) and `raudio` (audio playback) are not compiled in. Camera system, gestures, screen capture, compression API and automation events are off as well. This keeps the binary to the 2D drawing path.
- **Smaller limits.** File paths, key queues and rlgl batch sizes are sized for an MCU rather than a desktop.
- **PSRAM for big allocations.** The renderer allocates through `heap_caps_malloc()`, preferring PSRAM above the configured threshold, with internal RAM as a fallback in both directions. Nothing outside the component is affected.
- **File and directory APIs work through the VFS.** The component depends on esp-idf's `vfs`, so `LoadDirectoryFiles()`, `MakeDirectory()` and friends operate on any mounted filesystem (FAT on SD, LittleFS, ...). With none mounted they fail cleanly.
- **No upstream platform port.** raylib's own ports cover desktops, the web, DRM and Android; ESP-IDF gets `src/rcore_platform_esp_idf.c` instead, which supplies the same platform entry points and nothing else. See [How the component is put together](#how-the-component-is-put-together).

## How the component is put together

The public surface and the platform internals live apart:

- `include/esp_raylib.h` + `src/esp_raylib.c` are the public API: the display
  config struct and `esp_raylib_set_display()`. Everything a caller touches
  before `InitWindow()` stays in one ordinary source file.
- A configure-time copy of upstream's `rcore.c`, compiled from a shadow
  directory in the build tree, is the translation unit for the port side; the
  raylib target's own `rcore.c` is dropped. Upstream's ports are `#include`d
  into `rcore.c` rather than compiled next to it, because `CoreData`, the
  `RL_*` limits and the rlsw instance only exist inside that translation unit.
  A component cannot ask upstream's CMake to add a source to an existing
  target, so this file is compiled by the component. Compiling a copy --
  rather than the submodule's file -- relocates the directory that its quoted
  `#include "platforms/rcore_memory.c"` resolves against, which is what lets
  the port below take that slot without touching upstream. It also takes
  rcore.c's `PLATFORM_MEMORY` branch, silencing the "no platform defined"
  `#pragma message` that no compiler flag can suppress.
- `src/rcore_platform_esp_idf.c` is the port: it supplies the
  window, monitor, clipboard, cursor, input and timer functions raylib expects
  from a platform, and hands finished frames to `esp_lcd`. It sits exactly
  where upstream's own ports sit, at rcore.c's platform hook. Frames are
  presented by an internal task: the renderer queues a finished double-buffered
  frame and keeps drawing into the other buffer, so the panel transfer and the
  next frame's rendering overlap. `flush_cb` stays synchronous per band and is
  always called from that one task, which keeps the pixel-buffer contract
  simple. No raylib module is reimplemented.
- `src/esp_raylib_private.h` carries the state the two sides share (the
  registered display config and the `GetTime()` epoch); it is not installed.
- `CMakeLists.txt` drives upstream's own CMake and then applies the handful of
  settings ESP-IDF needs, through target properties only.

The build sets `PLATFORM=Memory` so upstream's CMake does not look for OpenGL or
GLFW. Memory's own C port is not used: it wraps every frame in a *second* full
RGBA copy, 300 KiB of nothing on this target. The shadow directory puts the
component's own port at that include slot instead.

## Examples

- [examples/raylib-fireworks](examples/raylib-fireworks) drives an ST7789 panel over SPI on an ESP32-S3 with a self-playing fireworks show over a night skyline.

## Documentation

This component does not re-document raylib. For the API, the examples and the tutorials, see:

- [raylib cheatsheet](https://www.raylib.com/cheatsheet/cheatsheet.html) — every function on one page
- [raylib examples](https://www.raylib.com/examples.html) — the canonical example set
- [raylib wiki](https://github.com/raysan5/raylib/wiki) — building, porting and internals

Keep in mind what is listed under "What this component changes" when following a desktop example.
