# LVGL Simulation: Render into an SDL Panel

This example renders an LVGL screen into an `esp_lcd_sim` panel, so the GUI can be developed and checked on a development machine without attaching an LCD. The flow of the example is the one of a regular GUI application: it only uses the generic `esp_lcd` panel API and does not know that the panel is simulated.

## What the example does

1. Creates a 240 x 320 `ESP_COLOR_FOURCC_BGR24` panel, shown inside a development-board frame in the SDL preview window (the LCD screen keeps the exact 240 x 320 resolution).
2. Registers the panel as an LVGL display (with the tick source from `esp_timer`) and renders a static screen with a title, a chart and a button.
3. Starts a background `esp_console` REPL task and runs the GUI loop on the main thread: with a preview window it refreshes until the user closes the window (or types `quit`); headless it keeps rendering while the REPL task feeds it commands, until `quit`.
4. Saves the rendered frame as `screenshot.png` through the component, stops the REPL task, deletes the panel, releases the SDL resources with `esp_lcd_sim_deinit()` and exits.
5. A pytest script reads that PNG file and compares it with the committed `golden_result.png`.

Keeping the UI static makes the output deterministic and suitable for golden-image testing; the interactive parts are the preview loop and the console REPL.

## Build and run

The panel is simulated on the machine running the application, so the example builds for the ESP-IDF host target, which is called `linux` on the operating systems the target supports (Linux and macOS). The build system skips the `flash` step for this target and `monitor` runs the binary, so the usual workflow works unchanged:

```bash
git submodule update --init --recursive ../../SDL
idf.py --preview set-target linux
idf.py build flash monitor
```

A window with the development board and the rendered screen inside it appears on the desktop. The board frame is only decoration: the saved `screenshot.png` contains just the 240 x 320 LCD content, not the surrounding board. Closing its title bar button stops the example: the frame is written to `screenshot.png` in the working directory and the application exits (which also ends `idf.py monitor`). The binary can also be run directly as `./build/lvgl_sim_sdl.elf`.

The example asks for a preview window but does not depend on one: when no display is available (a CI machine, a remote shell, or a build without the X11 and Wayland packages) the panel falls back to framebuffer-only with a warning, and the example detects it with `esp_lcd_sim_window_is_open()`. The render and the PNG export work the same way, which is what lets the CI run the example. Set `create_window` to `false` in [`main/lvgl_sim_sdl_main.c`](main/lvgl_sim_sdl_main.c) to never open a window.

The output contains messages similar to:

```text
I (350) example: Install the SDL simulated LCD panel driver
I (360) lcd_sim.sdl: preview window 'LVGL Example Simulator' created (304x432, scale 1, with board frame)
I (370) lcd_sim: SDL sim panel created (240x320, with preview window)
I (380) example: Run the GUI loop (close the window or type 'quit' to stop)
I (940) lcd_sim.sdl: preview window closed by the user
I (950) example: Save the rendered frame as a PNG file
I (960) lcd_sim.shot: Saved 240x320 PNG image (5311 bytes) to 'screenshot.png'
I (970) example: LVGL sim SDL example done.
```

## Automated verification

Run the test from this directory with a build directory created by the CI or by a local `idf.py build`:

```bash
pytest pytest_lvgl_sim_sdl.py --target linux --embedded-services idf --build-dir build
```

Application and test both run on the host, so the test reads `screenshot.png` from the working directory directly: no serial transfer, no base64 encoding and no intermediate image format. The test decodes the PNG with the standard library, checks the geometry and compares the pixels with `golden_result.png`. Copy the new `screenshot.png` over the golden image when the UI changed intentionally.

### Driving the GUI from the test

The example runs an [`esp_console`](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/console.html) REPL on a background task so a test can interact with the UI instead of only comparing a static frame. The commands themselves are provided by the component through `esp_lcd_sim_console_register()` (enabled by `CONFIG_ESP_LCD_SIM_CONSOLE=y` in [`sdkconfig.defaults`](sdkconfig.defaults)); their names and arguments are part of the component's public API and are documented in the [console command interface guide](../../docs/src/console.md). The example creates the REPL with `esp_console_new_repl_stdio()` + `esp_console_start_repl()`, registers the component's command set plus one application command of its own (`hello`), and keeps rendering LVGL on the main thread:

| Command | Effect |
|---|---|
| `touch <x> <y> <0\|1>` | inject a pointer state, then sleep so LVGL settles |
| `reset` | stop injecting, return to the real pointer |
| `wait <ms>` | sleep while the GUI runs without input |
| `frame <path>` | export the current frame as a PNG |
| `quit` | stop the GUI loop, write `screenshot.png` and exit |
| `hello` | an application command unrelated to the LCD |
| `help` | list the commands and their arguments |

The commands run on the **background REPL task** while LVGL renders on the main thread — the two-thread layout a real application uses. This is safe because the component's commands only call the thread-safe panel entry points (`inject_touch`, `inject_reset`, `screenshot_save_png`), which a per-panel mutex serializes against the GUI thread's drawing; they never touch SDL or LVGL. The component does not know LVGL and cannot drive it, so its `touch` command sleeps `touch_settle_ms` and `wait` sleeps the requested time, giving the independent GUI thread time to sample the input and advance before the next command (for example a `frame`) runs. The `hello` command shows that the application can register its own, unrelated commands on the same REPL.

`test_lvgl_sim_sdl_touch` uses the commands to press the demo button and asserts that only the click-counter label changed, while `test_lvgl_sim_sdl_example` sends `quit` and keeps the golden-image comparison. On `quit` (or a window close) the GUI loop stops, the frame is written to `screenshot.png`, the REPL task is stopped with `esp_console_stop_repl()` and the application exits.

The example keeps `CONFIG_VFS_SUPPORT_IO` disabled in [`sdkconfig.defaults`](sdkconfig.defaults): on the `linux` target the VFS layer interposes `read`/`write`/`select` but not `eventfd()`, which breaks the `esp_console_stop_repl()` teardown (see the note in the [console command interface guide](../../docs/src/console.md#application-skeleton)).

### Capturing a frame in an application

The generic `esp_lcd` interface has no concept of a complete GUI frame: a frame may consist of multiple partial flushes. Export the panel content only after the refresh you want is finished (for example after a synchronous `lv_refr_now()`, as in this example). The export snapshots the framebuffer under the panel lock and encodes outside it, so it is thread-safe and may run on the REPL task while the GUI thread keeps drawing; the snapshot is a consistent frame, not a torn one.

The PNG export only depends on the panel itself, so it also works when the panel was created with `create_window = false`, which is what the CI runs use.
