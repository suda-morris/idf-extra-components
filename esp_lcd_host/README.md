# Simulate a Display: SDL Backed Virtual `esp_lcd` Panel

[![Component Registry](https://components.espressif.com/components/espressif/esp_lcd_host/badge.svg)](https://components.espressif.com/components/espressif/esp_lcd_host)

`esp_lcd_host` is a virtual [`esp_lcd`](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/lcd.html) panel driver that renders into an [SDL](https://www.libsdl.org/) window on a development machine. When a GUI needs a display but wiring up a screen is inconvenient, the GUI port can be built against this panel instead of a real display controller:

- no LCD, no ribbon cable, no board: the same `idf.py build` builds the GUI against the simulated panel,
- the GUI code stays untouched, because the panel implements the generic `esp_lcd_panel_t` interface used by every LCD driver,
- the rendered frame can be saved as a PNG file and compared against a golden image in CI.

The component only builds for the ESP-IDF host (`linux`) target. The manifest of the component declares that target, so the component manager rejects the dependency for a real chip instead of failing later in the build.

## SDL

The component vendors SDL3 as the git submodule [`esp_lcd_host/SDL`](SDL) and builds it with the CMake project SDL ships, through `add_subdirectory()` in `port/sdl/CMakeLists.txt`. It is not taken from the component registry, and reusing the SDL project keeps the source lists and the build configuration header in sync with the submodule instead of copying them here.

Before adding the subdirectory, the port turns off what an LCD driver does not need and fixes the few results SDL would otherwise probe from the build machine:

- the video, event, thread, timer and filesystem paths, with the software renderer,
- the video drivers that open a window on a desktop: **X11 and Wayland**, plus **KMSDRM** when `libdrm`, `gbm` and EGL are installed (a window on a local console), plus the always available dummy and offscreen drivers for headless machines and CI,
- no audio, camera, joystick, haptic, hidapi, sensor, power, dialog, tray or GPU subsystem, no GPU video driver, and none of the optional dependencies (Fribidi, libthai, D-Bus, IBus, libudev, liburing) SDL would otherwise pick up from the machine running CMake.

The X11 and Wayland drivers are probed, not fixed: SDL looks for them with `pkg-config` and `find_package(X11)`, and each one is compiled in only when the build machine has its development packages. Which one is used is decided by SDL at run time, so the same binary works in a Wayland session (native Wayland), in an X11 session, and under XWayland.

An X11 driver that is half installed is handled too: the port probes the X11 packages before SDL does, so the driver is left out when they are incomplete instead of stopping the configuration, and the same for the X11 extensions SDL would otherwise require.

Both are on by default, because a screen simulator is expected to open its preview window on the machine that builds it. They can be turned off for a build that must not depend on the build machine at all - it falls back to the dummy, offscreen and KMSDRM drivers, exactly like before:

```bash
idf.py -DESP_LCD_HOST_SDL_X11=OFF -DESP_LCD_HOST_SDL_WAYLAND=OFF build
```

This is what the CI of this repository does, so the Linux build there only depends on the toolchain in the `espressif/idf` image.

The packages that are needed for a preview window on a desktop:

| Driver | Debian/Ubuntu packages |
| --- | --- |
| Wayland | `libwayland-dev wayland-protocols libxkbcommon-dev libegl-dev` |
| X11 | `libx11-dev libxext-dev` |
| KMSDRM | `libdrm-dev libgbm-dev libegl-dev` |

They are build time dependencies only: SDL loads X11 and Wayland with `dlopen()` at run time, so the executable is not linked against them and a machine without a display server still runs the same binary. Neither driver is required - without any of them the panel falls back to the offscreen driver and keeps working as an exportable framebuffer, which is what CI does.

SDL generates `SDL_build_config.h` during the build, so the port does not carry a hand written configuration.

Because SDL is built from the submodule, the component has no dependency other than ESP-IDF:

- the same `idf.py build` builds SDL and the panel, and nothing is downloaded at build time,
- there is no board support layer that would install a display of its own.

Check out the submodule after cloning the repository, or the build stops with an explicit error:

```bash
git submodule update --init --recursive esp_lcd_host/SDL
```

## Installation

Add the component to your project's `idf_component.yml`:

```yaml
dependencies:
  espressif/esp_lcd_host: "^0.1.0"
```

Then run `idf.py reconfigure` or build the project.

> The component requires the `linux` target and ESP-IDF `>= 6.0.0`.

## Quick start

Create the panel with the same resolution and pixel format as the framebuffer produced by your GUI library, initialize it, and pass the panel handle to the GUI display driver:

```c
#include "esp_lcd_host.h"

esp_lcd_host_config_t config = {
    .width = 240,
    .height = 240,
    .color_format = ESP_COLOR_FOURCC_BGR24,  // LVGL RGB888 in memory
    .create_window = true,                   // set to false on a machine without a display
    .scale = 2,                              // 240x240 panel in a 480x480 window
    .window_title = "My GUI",
};

esp_lcd_panel_handle_t panel = NULL;
ESP_ERROR_CHECK(esp_lcd_new_panel_host_sdl(&config, &panel));
ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

// Configure your GUI library to render/flush into `panel`.
```

Replace the panel creation in the board initialization of your GUI with the snippet above to move a display to the host. The rest of the GUI code, including the LVGL flush callback that calls `esp_lcd_panel_draw_bitmap()`, does not change.

While a preview window is open, pump its event queue periodically, for example from the LVGL timer handler:

```c
ESP_ERROR_CHECK(esp_lcd_host_pump_events());
```

### Save the rendered frame

```c
// Save the latest frame as PNG. The file uses the panel resolution and color
// format, a scaled preview window does not change it.
ESP_ERROR_CHECK(esp_lcd_host_screenshot_save_png(panel, "screenshot.png"));
```

See the example for a pytest script that compares the rendered frame against a golden image.

### Publishing frames to the simulator

Hand the frame over explicitly after the GUI finished the frame:

```c
ESP_ERROR_CHECK(esp_lcd_host_return_panel(panel));
ESP_ERROR_CHECK(esp_lcd_host_return_buffers(panel));
```

Both calls are accepted before and after the SDL panel exists.

## Example

[`examples/lvgl_host_sdl`](examples/lvgl_host_sdl) renders an LVGL screen into the SDL panel, opens a preview window and writes `screenshot.png`, which the pytest script of the example compares against the committed golden image:

```bash
cd examples/lvgl_host_sdl
idf.py set-target linux
idf.py build
./build/lvgl_host_sdl.elf
```

The application and the test both run on the host, so the test reads the PNG file the application wrote. There is no serial transfer and no intermediate image format.

## Notes and limitations

- The component only supports the `linux` target. There is no driver for a real display controller, use an `esp_lcd_*` panel driver for that.
- The panel stores the pixels submitted by the GUI, so rotation, gap, mirroring and color inversion are not applied to the exported image. The same is true for the preview window.
- A frame may consist of several partial flushes. Export or publish the panel content only after the refresh you want is finished (for example after a synchronous `lv_refr_now()`).
- A preview window needs a display: SDL opens it through its X11 or Wayland driver on a desktop, and through KMSDRM on a local console with access to the DRM device. Everywhere else, including a build without the X11 and Wayland development packages, SDL falls back to the dummy and offscreen drivers, and the panel keeps working as a framebuffer that can be exported as a PNG file.
- Preview scaling is handled by SDL. The exported PNG always uses the panel resolution.
