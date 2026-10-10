# LVGL on the Host: Render into an SDL Panel

This example renders an LVGL screen into an `esp_lcd_host` panel, so the GUI can be developed and checked on a development machine without attaching an LCD. The flow of the example is the one of a regular GUI application: it only uses the generic `esp_lcd` panel API and does not know that the panel is simulated.

## What the example does

1. Creates a 240 x 240 `ESP_COLOR_FOURCC_BGR24` panel, with a 2x scaled SDL preview window.
2. Registers the panel as an LVGL display (with the host tick source from `esp_timer`) and renders a static screen with a title, a chart and a button.
3. When the preview window opened, keeps running and refreshing it until the user closes the window. When the machine has no display, it waits for the frame to reach the framebuffer and stops after a few frames.
4. Saves the rendered frame as `screenshot.png` through the component and exits.
5. A pytest script reads that PNG file and compares it with the committed `golden_result.png`.

Keeping the UI static makes the output deterministic and suitable for golden-image testing; the interactive part is only the preview loop.

## Build and run

The panel is simulated on the machine running the application, so the example builds for the ESP-IDF host target, which is called `linux` on the operating systems the target supports (Linux and macOS). The build system skips the `flash` step for this target and `monitor` runs the binary, so the usual workflow works unchanged:

```bash
git submodule update --init --recursive ../../SDL
idf.py --preview set-target linux
idf.py build flash monitor
```

A window with the rendered screen appears on the desktop. Closing its title bar button stops the example: the frame is written to `screenshot.png` in the working directory and the application exits (which also ends `idf.py monitor`). The binary can also be run directly as `./build/lvgl_host_sdl.elf`.

The example asks for a preview window but does not depend on one: when no display is available (a CI machine, a remote shell, or a build without the X11 and Wayland packages) the window cannot be created, so the application logs a warning and creates the panel with `create_window = false` instead. The render and the PNG export work the same way, which is what lets the CI run the example. Set `create_window` to `false` in [`main/lvgl_host_sdl_main.c`](main/lvgl_host_sdl_main.c) to never open a window.

The output contains messages similar to:

```text
I (350) example: Install the SDL host LCD panel driver
I (360) lcd_host: Host SDL panel created (240x240, with preview window)
I (370) lcd_host.sdl: preview window 'esp_lcd_host example' created (480x480, scale 2)
I (380) example: Close the preview window to stop the example
I (940) lcd_host.sdl: preview window closed by the user
I (950) example: Save the rendered frame as a PNG file
I (960) lcd_host.shot: Saved 240x240 PNG image (5034 bytes) to 'screenshot.png'
I (970) example: LVGL host SDL example done.
```

## Automated verification

Run the test from this directory with a build directory created by the CI or by a local `idf.py build`:

```bash
pytest pytest_lvgl_host_sdl.py --target linux --embedded-services idf --build-dir build
```

Application and test both run on the host, so the test reads `screenshot.png` from the working directory directly: no serial transfer, no base64 encoding and no intermediate image format. The test decodes the PNG with the standard library, checks the geometry and compares the pixels with `golden_result.png`. Copy the new `screenshot.png` over the golden image when the UI changed intentionally.

### Capturing a frame in an application

The generic `esp_lcd` interface has no concept of a complete GUI frame: a frame may consist of multiple partial flushes. Export the panel content only after the refresh you want is finished (for example after a synchronous `lv_refr_now()`, as in this example). The driver does not pause drawing, so do not export from another thread while the GUI is still flushing that frame.

The PNG export only depends on the panel itself, so it also works when the panel was created with `create_window = false`, which is what the CI runs use.
