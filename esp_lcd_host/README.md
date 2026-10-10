# Simulate a Display: SDL Backed Virtual `esp_lcd` Panel

[![Component Registry](https://components.espressif.com/components/espressif/esp_lcd_host/badge.svg)](https://components.espressif.com/components/espressif/esp_lcd_host)

`esp_lcd_host` is a virtual [`esp_lcd`](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/lcd.html) panel driver that renders into a window on your development machine, so a GUI can be developed and tested without a screen. The preview window is drawn with [SDL](https://www.libsdl.org/), which is built into the application; nothing has to be installed at run time.

- No LCD, no ribbon cable, no board: the same `idf.py build` builds the GUI against the simulated panel.
- The GUI code stays untouched, because the panel implements the generic `esp_lcd_panel_t` interface that every LCD driver implements.
- The rendered frame can be saved as a PNG file and compared against a golden image in CI.

## Installation

Add the component to your project's `idf_component.yml`:

```yaml
dependencies:
  espressif/esp_lcd_host: "^0.1.0"
```

Then run `idf.py reconfigure` or build the project.

> Requires the ESP-IDF host (`linux`) target and ESP-IDF `>= 6.0.0`.

## Quick start

Create the panel with the same resolution and pixel format as the framebuffer your GUI library produces, initialize it, and pass the panel handle to the GUI display driver:

```c
#include "esp_lcd_host.h"

esp_lcd_host_config_t config = {
    .width = 240,
    .height = 240,
    .color_format = ESP_COLOR_FOURCC_BGR24,  // RGB888 in memory
    .create_window = true,                   // set to false on a machine without a display
    .scale = 2,                              // 240x240 panel in a 480x480 window
    .window_title = "My GUI",
};

esp_lcd_panel_handle_t panel = NULL;
ESP_ERROR_CHECK(esp_lcd_new_panel_host_sdl(&config, &panel));
ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

// Configure your GUI library to render/flush into `panel`.
```

Replace the panel creation in the board initialization of your GUI with the snippet above to move a display to the host. The rest of the GUI code, including the LVGL flush callback that calls `esp_lcd_panel_draw_bitmap()`, does not change.

### Run the preview

While a preview window is open, pump its event queue periodically, for example from the LVGL timer handler. The call presents the latest frame in every preview window and closes a window when the user closes it:

```c
while (!esp_lcd_host_window_close_requested()) {
    ESP_ERROR_CHECK(esp_lcd_host_pump_events());
    lv_timer_handler();          // your GUI tick
    lv_delay_ms(16);
}
```

### Save the rendered frame

```c
// Save the latest frame as PNG. The file uses the panel resolution and color
// format, a scaled preview window does not change it.
ESP_ERROR_CHECK(esp_lcd_host_screenshot_save_png(panel, "screenshot.png"));
```

## Example

[`examples/lvgl_host_sdl`](examples/lvgl_host_sdl) renders an LVGL screen into the panel, opens a preview window and writes `screenshot.png`, which the pytest script of the example compares against the committed golden image. See the [example README](examples/lvgl_host_sdl/README.md) for how to build and run it.

The application and the test both run on the host, so the test reads the PNG file the application wrote. There is no serial transfer and no intermediate image format.

## Notes and limitations

- The component only supports the ESP-IDF host (`linux`) target, on Linux and macOS. There is no driver for a real display controller, use an `esp_lcd_*` panel driver for that.
- The panel stores the pixels submitted by the GUI, so rotation, gap, mirroring and color inversion are not applied to the exported image. The same is true for the preview window.
- A frame may consist of several partial flushes. Export the panel content only after the refresh you want is finished (for example after a synchronous `lv_refr_now()`).
- A preview window needs a display. On a machine without one, including a build without the X11 and Wayland development packages, the panel still works as a framebuffer that can be exported as a PNG file. Set `create_window` to `false` for such builds.
- Preview scaling is handled by SDL, so the window can be resized freely. The exported PNG always uses the panel resolution.
