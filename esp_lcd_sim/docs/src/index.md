# Getting Started

`esp_lcd_sim` is a virtual [`esp_lcd`](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/lcd.html) panel driver for the ESP-IDF host (`linux`) target. Instead of driving a display controller over SPI or RGB, it stores the pixels in a framebuffer and shows them in an [SDL](https://www.libsdl.org/) window on your development machine.

Because it implements the generic `esp_lcd_panel_t` interface, your GUI code does not change: the same LVGL (or other GUI library) port that runs on a real board runs against the simulated panel. Only the panel creation in your board initialization differs.

What you get:

- **No hardware needed.** The same `idf.py build` builds the GUI for your development machine; no LCD, no ribbon cable, no board.
- **The mouse is the finger.** Click and drag in the preview window to drive the touch input of your GUI.
- **Frames as PNG files.** Export the framebuffer and compare it against a golden image in CI.
- **Scriptable input.** Inject touches from code or from a command line, so a test can click a button and verify the result — with or without a window.

SDL is vendored in the component and built into the application; nothing has to be installed at run time.

## Installation

Add the component to your project's `idf_component.yml`:

```yaml
dependencies:
  espressif/esp_lcd_sim: "^0.1.0"
```

Then run `idf.py reconfigure` or build the project. The component requires the ESP-IDF host (`linux`) target and ESP-IDF `>= 6.0.0`, on Linux or macOS.

## Create Your First Panel

Create the panel with the same resolution and pixel format as the framebuffer your GUI library produces, then initialize it:

```c
#include "esp_lcd_sim.h"

esp_lcd_sim_config_t config = {
    .width = 240,
    .height = 320,
    .color_format = ESP_COLOR_FOURCC_BGR24,  // RGB888, stored as B, G, R in memory
    .create_window = true,                   // open an SDL preview window
    .window_title = "My GUI",
};

esp_lcd_panel_handle_t panel = NULL;
ESP_ERROR_CHECK(esp_lcd_new_panel_sim_sdl(&config, &panel));
ESP_ERROR_CHECK(esp_lcd_panel_init(panel));   // clears the framebuffer to black

// Pass `panel` to your GUI library instead of a real LCD driver.
```

From here on, the panel is used through the generic `esp_lcd` API: your flush callback calls `esp_lcd_panel_draw_bitmap(panel, ...)`, exactly as it would with a physical display driver. See [Porting an LVGL Application](lvgl_port.md) for a complete port.

### Pixel Formats

The `color_format` must match the byte layout your GUI writes into the draw buffer:

| `color_format` | Layout in memory | Typical source |
|---|---|---|
| `ESP_COLOR_FOURCC_RGB16` | RGB565, little endian | 16-bit GUI color depth |
| `ESP_COLOR_FOURCC_RGB16_BE` | RGB565, big endian | 16-bit, byte swapped |
| `ESP_COLOR_FOURCC_RGB24` | R, G, B | RGB888 |
| `ESP_COLOR_FOURCC_BGR24` | B, G, R | LVGL `LV_COLOR_FORMAT_RGB888` |
| `ESP_COLOR_FOURCC_BGRA32` | B, G, R, A | LVGL `LV_COLOR_FORMAT_ARGB8888` |

Other fourcc values are rejected at creation time.

### Preview Options

The remaining configuration fields only affect the preview window, never the framebuffer or the exported PNG:

- `scale` — integer upscale factor of the preview (`0` or `1` means no scaling). The window can also be resized freely; SDL scales with nearest neighbor, so panel pixels stay sharp.
- `show_board_frame` — draw the LCD inside a fixed development-board frame instead of a bare window. The screen cut-out keeps the exact panel resolution, so the GUI appears 1:1 inside it. See [Board Frame](#board-frame).
- `screenshot_key` / `screenshot_path` — press a key in the preview to save a PNG. See [Screenshots and Golden Images](screenshot.md).

## Run the Preview Loop

While a preview window is open, drive it from your GUI loop. Each iteration pumps the SDL event queue and presents what the GUI has drawn since the previous iteration:

```c
while (esp_lcd_sim_window_is_open(panel)) {
    ESP_ERROR_CHECK(esp_lcd_sim_pump_events());
    uint32_t idle_ms = lv_timer_handler();   // run the GUI timers
    esp_lcd_sim_wait_events(idle_ms);        // sleep until input or the next timer
}
```

- `esp_lcd_sim_pump_events()` drains the SDL event queue (acting on quit, key-down and window-close events) and presents every window that was drawn to. Call it periodically, for example once per GUI timer run.
- `esp_lcd_sim_wait_events(timeout_ms)` blocks until a preview event arrives or the timeout expires, so an idle GUI costs almost no CPU and still reacts to input promptly. Passing the GUI library's "time until next timer" value (as `lv_timer_handler()` returns) keeps animations on schedule. Without a preview window it sleeps for the timeout instead, so the same call idles a headless run — see [Headless Runs](#headless-runs-no-display).
- `esp_lcd_sim_window_is_open(panel)` becomes false when the user closes the panel's preview window (or quits through the window manager). The window is never reopened, so a periodic check cannot miss the close.

All three functions belong to the **GUI thread** — the same thread that creates the panel and calls `esp_lcd_panel_draw_bitmap()`. See [Thread Safety and Lifecycle](threading.md) for the full model.

## Headless Runs (No Display)

A preview window needs a display. When none is available — a CI machine, or a build without the X11/Wayland development packages — the panel logs a warning and falls back to **framebuffer-only**: rendering and PNG export work exactly the same, only the window is missing. Check which path you got:

```c
bool with_window = esp_lcd_sim_window_is_open(panel);
```

Set `create_window = false` to skip the window attempt altogether.

> **Tip:** to force the headless path without touching the code — for example in a test — set `SDL_VIDEODRIVER` to a driver that does not exist (`SDL_VIDEODRIVER=nonexistent`). Window creation then fails and the panel falls back to the framebuffer. The example test (`examples/lvgl_sim_sdl/pytest_lvgl_sim_sdl.py`) uses this to run deterministically on desktops and CI alike.

Without a window there is no SDL event queue, so `esp_lcd_sim_wait_events()` sleeps for its timeout instead of returning at once. The loop body is the same in both modes; only the condition differs, because a headless run has no window to close and stops on its own flag:

```c
while (!quit && (!with_window || esp_lcd_sim_window_is_open(panel))) {
    ESP_ERROR_CHECK(esp_lcd_sim_pump_events());
    uint32_t idle_ms = lv_timer_handler();
    if (!with_window) {
        idle_ms = idle_ms > 20 ? 20 : idle_ms;   // stay responsive to injection
    }
    esp_lcd_sim_wait_events(idle_ms);
}
```

Headless the wait is a sleep that an injected touch (see [Touch Input](input.md)) cannot interrupt, so cap it to keep input responsive even when the GUI reports a long idle. The sleep is rounded up to whole FreeRTOS ticks, so a timeout shorter than one tick still yields the CPU instead of spinning. With a window the wait ends on the next preview event and needs no bound.

## Board Frame

With `show_board_frame = true` the preview shows the LCD as the screen of a development board: PCB, bezel, silkscreen name, power LED and mounting holes, drawn once at creation. The look is fixed and deliberately shows only what means something in the simulation — connectors, pin headers, buttons and an MCU package are not drawn, because nothing in the simulation reacts to them.

The screen cut-out keeps the **exact** panel resolution: a 240x320 panel shows a 240x320 screen, composited 1:1. `scale` and window resizing enlarge or shrink the whole board, not the panel pixels.

The board is decorative and preview-only:

- The framebuffer never contains the board; `esp_lcd_sim_screenshot_save_png()` still exports just the LCD content at the configured resolution.
- If the board texture cannot be built, the preview falls back to a plain LCD view and the panel keeps working.
- The field has no effect when `create_window = false`.

## Teardown

Delete every panel first, then release the component's global SDL resources once, before the process exits:

```c
ESP_ERROR_CHECK(esp_lcd_panel_del(panel));   // all panels first
ESP_ERROR_CHECK(esp_lcd_sim_deinit());       // then SDL, once
```

`esp_lcd_sim_deinit()` quits SDL as a whole. Do not call it while another part of the process still uses SDL, and do not call it while any sim panel is still alive (it returns `ESP_ERR_INVALID_STATE` then). If your application runs a console REPL or another thread that uses the panel, stop those threads before deleting the panel — see [Thread Safety and Lifecycle](threading.md).

## Limitations

- The component only supports the ESP-IDF host (`linux`) target, on Linux and macOS. For a real display controller, use an `esp_lcd_*` panel driver.
- The panel stores the pixels exactly as the GUI submits them: rotation, gap, mirroring and color inversion are applied by neither the preview nor the PNG export.
- A GUI frame may consist of several partial flushes. Export the panel content only after the refresh you want is finished — see [Screenshots and Golden Images](screenshot.md).
- SDL's event queue is process-wide. `esp_lcd_sim_pump_events()` drains it; an application that also uses SDL must not run a second, concurrent event loop. Keyboard input is not forwarded to the GUI; the configured screenshot key is consumed by the component.

### Preview Window Environment

- On X11, SDL checks for its optional extensions (Xcursor, XInput2, Xrandr and friends) at configure time. If a development package is missing, SDL stops with a clear error naming the package to install (or the `-DSDL_X11_<EXT>=OFF` switch to skip that extension), rather than silently building a reduced preview. The Wayland driver is unaffected.
- A preview window on a native Wayland session needs an ESP-IDF whose FreeRTOS Linux port passes per-call `MSG_DONTWAIT` through (fixed in ESP-IDF 6.2); older versions deadlock inside the first Wayland roundtrip before any window appears. X11 sessions (including XWayland) are not affected.
- On Wayland, SDL asks the compositor for decorations via `xdg-decoration` and falls back to libdecor (loaded dynamically). A compositor that supports neither produces a window without a title bar; close it with the compositor's close shortcut.

## Where to Go Next

- [Porting an LVGL Application](lvgl_port.md) — a complete, runnable GUI port.
- [Touch Input](input.md) — the mouse as finger, and programmatic injection for tests.
- [Screenshots and Golden Images](screenshot.md) — PNG export and image comparison in CI.
- [Console Command Interface](console.md) — drive the GUI from a command line or pytest.
- [Thread Safety and Lifecycle](threading.md) — which API runs where, and why.
- [API Reference](api.md) — generated from the driver headers.

A complete application lives in `examples/lvgl_sim_sdl` (in the component repository): LVGL rendering, preview window, console command interface and a pytest that compares the rendered frame against a golden image.
