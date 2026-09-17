# LVGL on the Host: Render into an SDL Panel

This example renders an LVGL screen into an `esp_lcd_host` panel, so the GUI can be developed and checked on a development machine without attaching an LCD. The flux of the example is the one of a regular GUI application: it only uses the generic `esp_lcd` panel API and does not know that the panel is simulated.

## What the example does

1. Creates a 240 x 240 `ESP_COLOR_FOURCC_BGR24` panel, with a 2x scaled SDL preview window.
2. Registers the panel as an LVGL display and renders a static screen with a title, a chart and a button.
3. Saves the rendered frame as `screenshot.png` (through the component) and `lvgl_host_sdl_result.ppm` (the same frame, kept by the example itself).
4. Uses a pytest script that checks the PNG written by the component and compares the PPM frame with `golden_result.ppm`.

The reference framebuffer inside the example is filled by the same LVGL flush callback that feeds the panel, so the comparison does not depend on the SDL backend, which is not compiled for chip targets.

The screen is rendered once, without an LVGL task or tick timer. Keeping the UI static makes the output deterministic and suitable for golden-image testing.

## Prerequisites

The SDL component brings its own board abstraction layer, which must be told not to initialize a board display, because this example provides the display itself. [`sdkconfig.defaults`](sdkconfig.defaults) already sets:

```
CONFIG_SDL_BSP_NO_BOARD=y
```

## Build and run

The panel is simulated on the machine running the application, so the example builds for the ESP-IDF `linux` target:

```bash
idf.py set-target linux
idf.py build
./build/lvgl_host_sdl.elf
```

A window with the rendered screen appears. Press the window close button or `Ctrl+C` in the terminal to stop the application. Set `create_window` to `false` in [`main/lvgl_host_sdl_main.c`](main/lvgl_host_sdl_main.c) to run without a display, for example on a CI machine.

The serial output contains messages similar to:

```text
I (350) example: Install the SDL host LCD panel driver
I (360) lcd_host: Host SDL panel created (240x240, with preview window)
I (370) lcd_host.sdl: preview window 'esp_lcd_host example' created (480x480, scale 2)
I (600) example: Save the rendered frame as a PNG file
I (650) lcd_host.shot: Saved 240x240 PNG image (4321 bytes) to 'screenshot.png'
I (660) example: Reference frame written to lvgl_host_sdl_result.ppm
I (1200) example: LVGL host SDL example done.
```

The framebuffer is RGB888 in B, G, R byte order and the PNG file contains `240 x 240 x 3` bytes.

## Automated verification

Run the test from this directory with a build directory created by the CI or by a local `idf.py build`:

```bash
pytest pytest_lvgl_host_sdl.py --target linux --embedded-services idf --build-dir build
```

The pytest script checks the geometry and the color type of the PNG written by the component, saves it as `screenshot.png` in the pytest-embedded log directory (typically below `/tmp/pytest-embedded/`), and compares the PPM frame with `golden_result.ppm`. Copy the saved `screenshot.png` over the golden image when the UI changed intentionally.

### Capturing a frame in an application

The generic `esp_lcd` interface has no concept of a complete GUI frame: a frame may consist of multiple partial flushes. Export the panel content only after the refresh you want is finished (for example after a synchronous `lv_refr_now()`, as in this example). The driver does not pause drawing, so do not export from another task while the GUI is still flushing that frame.

The PNG export only depends on the panel itself, so it also works when the panel was created with `create_window = false`, which is what the CI runs use.
