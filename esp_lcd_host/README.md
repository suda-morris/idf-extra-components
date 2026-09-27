# Simulate a Display: SDL Backed Virtual `esp_lcd` Panel

[![Component Registry](https://components.espressif.com/components/espressif/esp_lcd_host/badge.svg)](https://components.espressif.com/components/espressif/esp_lcd_host)

`esp_lcd_host` is a virtual [`esp_lcd`](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/lcd.html) panel driver that renders into an [SDL](https://www.libsdl.org/) window on a development machine. When a GUI needs a display but wiring up a screen is inconvenient, the GUI port can be built against this panel instead of a real display controller:

- no LCD, no ribbon cable, no board: the same `idf.py build` builds the GUI against the simulated panel,
- the GUI code stays untouched, because the panel implements the generic `esp_lcd_panel_t` interface used by every LCD driver,
- the rendered frame can be saved as a PNG file and compared against a golden image in CI.

The component builds for the ESP-IDF host target, which the manifest declares as `linux`. `linux` is the name ESP-IDF gives that target on **every** operating system - it does not mean that the component needs the Linux kernel. The same component builds and runs natively on **Linux, macOS and Windows**; declaring the target only makes the component manager reject the dependency for a real chip instead of failing later in the build.

## SDL

The component vendors SDL3 as the git submodule [`esp_lcd_host/SDL`](SDL) and builds it with the CMake project SDL ships, through `add_subdirectory()` in `port/sdl/CMakeLists.txt`. It is not taken from the component registry, and reusing the SDL project keeps the source lists and the build configuration header in sync with the submodule instead of copying them here.

The port reads the host platform from `CMAKE_SYSTEM_NAME` and then hands the platform specific work to the SDL project. SDL selects the window driver, the thread back end, the file system back end and the platform sources on its own, and it probes the machine for everything that describes the platform: the C library, pthreads, inotify, the compiler, the X11 extensions, Wayland, KMSDRM and EGL. `port/sdl/CMakeLists.txt` does not repeat any of that, so updating the submodule is the only thing that can change it.

What the port does do, before the subdirectory is added:

- turn off the subsystems an LCD panel does not use, so SDL skips their sources, their checks and their configuration macros: audio, camera, joystick, haptic, hidapi, power, sensor, dialog, tray, GPU, and the board and GPU video drivers.
- keep the video, event, thread, timer, file system and storage paths with the software renderer, plus the dummy and offscreen drivers that back the panel on a machine without a display.
- select the window drivers that open the preview window. X11, Wayland and KMSDRM are on by default; SDL probes for the packages of each one and leaves the driver out when they are missing.
- answer the checks whose result depends on the machine running CMake rather than on the platform. Those are the optional third party libraries SDL would otherwise pick up from whatever happens to be installed (Fribidi, libthai, D-Bus, IBus, libudev, liburing): the panel uses none of them, and probing for them would make the build machine dependent.

Everything the port keeps is a superset of what a desktop build of SDL would produce, so the only thing that can go wrong is more drivers being available than needed. The panel never selects a driver it does not ask for.

### Window drivers

The point of the component is to use the panel as a screen simulator, so a preview window has to open on the machine that builds it. All three desktop drivers of SDL are enabled by default:

| Driver | Option | Runs on |
| --- | --- | --- |
| Wayland | `ESP_LCD_HOST_SDL_WAYLAND` | a Wayland session, using the native Wayland back end |
| X11 | `ESP_LCD_HOST_SDL_X11` | an X11 session and under XWayland |
| KMSDRM | `ESP_LCD_HOST_SDL_KMSDRM` | a Linux console with access to the DRM device |

Each one is compiled in only when the machine has its development packages, and SDL picks the right one at run time, so the same binary works in every session. Turning a driver off is what a build wants when it must not depend on the build machine at all:

```bash
idf.py -DESP_LCD_HOST_SDL_X11=OFF -DESP_LCD_HOST_SDL_WAYLAND=OFF build
```

A build without any of the three still produces a working panel: SDL keeps the dummy and offscreen drivers, and the panel works as a frame buffer that can be exported as a PNG file.

SDL stops the configuration when the X11 driver is on and the X11 development packages are incomplete, and names the missing package and the option to turn off. Note that XInput2 and XFixes have to be turned off together, because SDL links XInput2 against XFixes.

The drivers are loaded with `dlopen()` at run time, so the executable is not linked against them and a machine without a display server still runs the same binary.

### Host platforms

| Host | Window driver | Packages for a preview window |
| --- | --- | --- |
| Linux | Wayland, X11, KMSDRM | see the table below (Debian/Ubuntu names) |
| macOS | Cocoa | none, the frameworks come with the system |
| Windows | Win32 | none, the libraries come with the system |

On Linux the desktop packages that are needed for a preview window are:

| Driver | Debian/Ubuntu packages |
| --- | --- |
| Wayland | `libwayland-dev wayland-protocols libxkbcommon-dev libegl-dev` |
| X11 | `libx11-dev libxext-dev libxrandr-dev libxfixes-dev libxcursor-dev libxi-dev libxtst-dev libxss-dev` |
| KMSDRM | `libdrm-dev libgbm-dev libegl-dev` |

They are build time dependencies only: SDL loads the drivers with `dlopen()` at run time.

The Linux desktop drivers are verified. The macOS and Windows branches of the port only select the window driver of the platform through the SDL project; no runner in this repository builds them, so treat them as unverified until one does.

### The generated configuration

SDL generates `SDL_build_config.h` during the build, so the port does not carry a hand written configuration. The port also does not carry a source list: `port/sdl/CMakeLists.txt` and `port/src/sdl_port_stubs.c` are the only files to review when the submodule is updated.

Because SDL is built from the submodule, the component has no dependency other than ESP-IDF and `espressif/libpng`:

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

> The component requires the ESP-IDF host (`linux`) target and ESP-IDF `>= 6.0.0`. The host target exists on Linux, macOS and Windows.

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

- The component only supports the ESP-IDF host (`linux`) target, on Linux, macOS and Windows. There is no driver for a real display controller, use an `esp_lcd_*` panel driver for that.
- The panel stores the pixels submitted by the GUI, so rotation, gap, mirroring and color inversion are not applied to the exported image. The same is true for the preview window.
- A frame may consist of several partial flushes. Export or publish the panel content only after the refresh you want is finished (for example after a synchronous `lv_refr_now()`).
- A preview window needs a display: SDL opens it through the native driver of the platform (X11, Wayland or Cocoa on a desktop, KMSDRM on a Linux console with access to the DRM device, Win32 on Windows). Everywhere else, including a build without the X11 and Wayland development packages, SDL falls back to the dummy and offscreen drivers, and the panel keeps working as a framebuffer that can be exported as a PNG file.
- Preview scaling is handled by SDL. The exported PNG always uses the panel resolution.
