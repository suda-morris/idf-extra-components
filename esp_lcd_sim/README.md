# Simulate a Display: SDL Backed Virtual `esp_lcd` Panel

[![Component Registry](https://components.espressif.com/components/espressif/esp_lcd_sim/badge.svg)](https://components.espressif.com/components/espressif/esp_lcd_sim)

`esp_lcd_sim` is a virtual [`esp_lcd`](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/lcd.html) panel driver that renders into a window on your development machine, so a GUI can be developed and tested without a screen. The preview window is drawn with [SDL](https://www.libsdl.org/), which is built into the application; nothing has to be installed at run time.

- No LCD, no ribbon cable, no board: the same `idf.py build` builds the GUI against the simulated panel.
- The GUI code stays untouched, because the panel implements the generic `esp_lcd_panel_t` interface that every LCD driver implements.
- The host mouse acts as the touch input, so widgets can be pressed and hovered in the preview window.
- The rendered frame can be saved as a PNG file and compared against a golden image in CI.

## Installation

Add the component to your project's `idf_component.yml`:

```yaml
dependencies:
  espressif/esp_lcd_sim: "^0.2.0"
```

Then run `idf.py reconfigure` or build the project.

> Requires the ESP-IDF host (`linux`) target and ESP-IDF `>= 6.0.0`.

## Quick start

Create the panel with the same resolution and pixel format as the framebuffer your GUI library produces, initialize it, and pass the panel handle to the GUI display driver:

```c
#include "esp_lcd_sim.h"

esp_lcd_sim_config_t config = {
    .width = 240,
    .height = 240,
    .color_format = ESP_COLOR_FOURCC_BGR24,  // RGB888 in memory
    .create_window = true,                   // without a display the panel falls back to framebuffer-only
    .show_board_frame = true,                // draw a development board with the LCD as its screen
    .scale = 1,                              // the LCD screen stays 1:1 (240x240 logical pixels)
    .window_title = "My GUI",
};

esp_lcd_panel_handle_t panel = NULL;
ESP_ERROR_CHECK(esp_lcd_new_panel_sim_sdl(&config, &panel));
ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

// Configure your GUI library to render/flush into `panel`.
```

Replace the panel creation in the board initialization of your GUI with the snippet above to simulate the display on your development machine. The rest of the GUI code, including the LVGL flush callback that calls `esp_lcd_panel_draw_bitmap()`, does not change.

### Run the preview

While a preview window is open, drive it from your GUI loop: pump the event queue, run the GUI timers, then wait for input or the next timer. `esp_lcd_sim_pump_events()` presents the latest frame in every preview window and closes a window when the user closes it; `esp_lcd_sim_wait_events()` blocks until a preview event arrives or the timeout expires, so an idle screen costs almost no CPU:

```c
while (!esp_lcd_sim_window_close_requested()) {
    ESP_ERROR_CHECK(esp_lcd_sim_pump_events());
    esp_lcd_sim_wait_events(lv_timer_handler());   // ms until the next GUI timer
}
```

The component initializes the SDL video subsystem when the first preview window is created. Delete every panel and then call `esp_lcd_sim_deinit()` once before the process exits (or before using SDL for something else); it also clears the close-requested flag:

```c
ESP_ERROR_CHECK(esp_lcd_panel_del(panel));   // all panels first
ESP_ERROR_CHECK(esp_lcd_sim_deinit());
```

### Development-board frame (optional)

Set `show_board_frame = true` to render the LCD inside a pre-rendered development board instead of a bare window. The board is drawn once at creation (PCB, display bezel, silkscreen name, power LED and mounting holes) and the screen cut-out keeps the **exact** panel resolution: the GUI is composited into that screen rectangle 1:1, so a 240x320 panel shows a 240x320 screen. Enlarge or shrink the whole board with `scale` or by resizing the window; the LCD pixels stay sharp (nearest-neighbor).

The board look is fixed. It deliberately shows only what means something in the simulation: the screen is the display, the rest is a symmetric frame around it. Connectors, pin headers, buttons and an MCU package are not drawn, because nothing in the simulation reacts to them.

This only changes the **preview window**. The panel framebuffer never contains the board, so `esp_lcd_sim_screenshot_save_png()` still exports just the LCD content at the configured resolution, with no board frame. The board is decorative: if it cannot be rendered, the preview falls back to a plain LCD view and the panel keeps working.

`show_board_frame` has no effect when `create_window = false`.

### Pointer input

The host mouse plays the finger on the simulated display: `esp_lcd_sim_mouse_get()` maps the pointer of the preview window into panel pixels and reports the left button as the touch contact. Feed it to your GUI library's input device — for LVGL that is a pointer indev whose read callback polls the panel (the example does exactly this, so the button in the demo UI can be pressed):

```c
static void mouse_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    esp_lcd_sim_mouse_data_t mouse = {0};
    esp_lcd_panel_handle_t panel = lv_indev_get_user_data(indev);
    if (esp_lcd_sim_mouse_get(panel, &mouse) == ESP_OK) {
        data->point.x = mouse.x;
        data->point.y = mouse.y;
        data->state = mouse.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    }
}
```

While the pointer is over the screen area (the LCD cut-out when `show_board_frame` is set, the whole window otherwise), `x`/`y` hold panel pixels. Off the glass the touch releases and the last on-screen position is kept, so hover styles keep working.

### Event callbacks

The panel reports the moment a GUI port knows from real transfer-based display drivers: `on_flush_done` fires after each `esp_lcd_panel_draw_bitmap()` has copied its pixels into the panel framebuffer — the stand-in for the color transfer done event of an SPI LCD (`on_color_trans_done`), where a port calls `lv_display_flush_ready()`:

```c
static void on_flush_done(esp_lcd_panel_handle_t panel, void *user_ctx)
{
    lv_display_flush_ready((lv_display_t *)user_ctx);   // same spot as a DMA completion callback
}

esp_lcd_sim_register_event_callbacks(panel, &(esp_lcd_sim_event_callbacks_t) {
    .on_flush_done = on_flush_done,
}, display);
```

It runs synchronously on the drawing thread, once per draw call (a partial refresh flushes several areas).

### Save the rendered frame

```c
// Save the latest frame as PNG. The file uses the panel resolution and color
// format, a scaled preview window does not change it.
ESP_ERROR_CHECK(esp_lcd_sim_screenshot_save_png(panel, "screenshot.png"));
```

The explicit call always writes the exact path it is given, which is what a
test comparing against a golden image wants. The preview's screenshot key
(`screenshot_key`) is friendlier for interactive use: it never overwrites. A
press writes `screenshot_path` (default `screenshot.png`) while that name is
free; once it exists, presses write the first free numbered variant,
`screenshot_001.png`, `screenshot_002.png`, and so on.

## Example

[`examples/lvgl_sim_sdl`](examples/lvgl_sim_sdl) renders an LVGL screen into the panel, opens a preview window and writes `screenshot.png`, which the pytest script of the example compares against the committed golden image. See the [example README](examples/lvgl_sim_sdl/README.md) for how to build and run it.

The application and the test both run on the host, so the test reads the PNG file the application wrote. There is no serial transfer and no intermediate image format.

## Notes and limitations

- The component only supports the ESP-IDF host (`linux`) target, on Linux and macOS. There is no driver for a real display controller, use an `esp_lcd_*` panel driver for that.
- Supported framebuffer formats: `ESP_COLOR_FOURCC_RGB16` and `ESP_COLOR_FOURCC_RGB16_BE` (RGB565), `ESP_COLOR_FOURCC_RGB24` (RGB888), `ESP_COLOR_FOURCC_BGR24` (RGB888, B,G,R in memory) and `ESP_COLOR_FOURCC_BGRA32` (ARGB8888 as LVGL stores it). Other fourcc values of `esp_lcd_types.h` are rejected at creation time.
- The panel stores the pixels submitted by the GUI, so rotation, gap, mirroring and color inversion are not applied to the exported image. The same is true for the preview window.
- A frame may consist of several partial flushes. Export the panel content only after the refresh you want is finished (for example after a synchronous `lv_refr_now()`).
- Create the panels, draw into them and call `esp_lcd_sim_pump_events()` from one thread: SDL windows and its event queue are not thread-safe, and the component adds no locking of its own.
- SDL's event queue is process-wide. `esp_lcd_sim_pump_events()` drains it, acting on quit, key-down and window-close-request events; an application that also uses SDL should not run a second, concurrent event loop. Pointer input does not come from the queue: `esp_lcd_sim_mouse_get()` samples SDL's cached mouse state. Keyboard input is not forwarded to the GUI; the configured screenshot key is consumed by the component itself.
- Call `esp_lcd_sim_deinit()` after deleting every panel. It quits SDL as a whole, so do not call it while another part of the process still uses SDL. See [Run the preview](#run-the-preview).
- A preview window needs a display. On a machine without one, including a build without the X11 and Wayland development packages, the panel falls back to framebuffer-only with a warning and can still export PNG files; `esp_lcd_sim_window_is_open()` reports `false`. Set `create_window` to `false` to skip the attempt.
- On X11, SDL enables its optional extensions (Xcursor, XInput2, Xrandr and friends) by default and checks for their development headers at configure time. If one is missing, SDL stops with a clear error naming the package to install (or the `-DSDL_X11_<EXT>=OFF` switch to skip that extension), rather than silently building a reduced preview. The Wayland driver is unaffected.
- A preview window on a native Wayland session needs an ESP-IDF whose FreeRTOS Linux port passes per-call `MSG_DONTWAIT` through (fixed in ESP-IDF 6.2); older versions deadlock inside the first Wayland roundtrip before any window appears. X11 sessions (including XWayland) are not affected.
- On Wayland, SDL asks the compositor for decorations via `xdg-decoration` and falls back to libdecor (loaded dynamically, so no dependency is added when the library is missing). A compositor that supports neither produces a window without a title bar; close it with the compositor's close shortcut.
- With `show_board_frame`, the preview window is larger than the panel because it also shows the board; `scale` applies to the whole scene. The exported PNG is unaffected and always uses the panel resolution.
- Preview scaling is handled by SDL with nearest neighbor sampling, so the window can be resized freely and the panel pixels stay sharp. SDL picks the renderer (an accelerated one when available, software as fallback); the exported PNG always uses the panel resolution and never passes through SDL, so it is independent of that choice.
