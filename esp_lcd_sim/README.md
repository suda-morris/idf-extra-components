# Simulate a Display: SDL Backed Virtual `esp_lcd` Panel

[![Component Registry](https://components.espressif.com/components/espressif/esp_lcd_sim/badge.svg)](https://components.espressif.com/components/espressif/esp_lcd_sim)

`esp_lcd_sim` is a virtual [`esp_lcd`](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/lcd.html) panel driver that renders into a window on your development machine, so a GUI can be developed and tested without a screen. The preview window is drawn with [SDL](https://www.libsdl.org/), which is built into the application; nothing has to be installed at run time.

- No LCD, no ribbon cable, no board: the same `idf.py build` builds the GUI against the simulated panel.
- The GUI code stays untouched, because the panel implements the generic `esp_lcd_panel_t` interface that every LCD driver implements.
- The host mouse acts as the touch input, so widgets can be pressed and hovered in the preview window.
- The rendered frame can be saved as a PNG file and compared against a golden image in CI.
- Touches can be injected from code or from a command line, so a test can click a button and verify the result — with or without a window.

## Documentation

- **[Programming Guide & API Reference](https://espressif.github.io/idf-extra-components/latest/esp_lcd_sim/index.html)** — getting started, LVGL porting, touch input, screenshots, console commands, thread safety.
- The guide sources live in [`docs/src`](docs/src/index.md); see the [repository README](../README.md#build-documentation-locally) for how to build them locally.

## Installation

Add the component to your project's `idf_component.yml`:

```yaml
dependencies:
  espressif/esp_lcd_sim: "^0.1.0"
```

Then run `idf.py reconfigure` or build the project.

> Requires the ESP-IDF host (`linux`) target and ESP-IDF `>= 6.0.0`, on Linux or macOS.

## Quick Start

Create the panel with the same resolution and pixel format as the framebuffer your GUI library produces, and pass the handle to your GUI display driver instead of a real LCD:

```c
#include "esp_lcd_sim.h"

esp_lcd_sim_config_t config = {
    .width = 240,
    .height = 320,
    .color_format = ESP_COLOR_FOURCC_BGR24,  // RGB888, stored as B, G, R in memory
    .create_window = true,                   // without a display the panel falls back to framebuffer-only
    .show_board_frame = true,                // draw the LCD inside a development-board frame
    .window_title = "My GUI",
    .screenshot_key = 's',                   // press S in the preview to save a PNG
};

esp_lcd_panel_handle_t panel = NULL;
ESP_ERROR_CHECK(esp_lcd_new_panel_sim_sdl(&config, &panel));
ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

// ... your GUI renders into `panel` via esp_lcd_panel_draw_bitmap() ...

while (esp_lcd_sim_window_is_open(panel)) {
    ESP_ERROR_CHECK(esp_lcd_sim_pump_events());
    esp_lcd_sim_wait_events(lv_timer_handler());   // ms until the next GUI timer
}

ESP_ERROR_CHECK(esp_lcd_panel_del(panel));
ESP_ERROR_CHECK(esp_lcd_sim_deinit());
```

The [LVGL porting guide](https://espressif.github.io/idf-extra-components/latest/esp_lcd_sim/lvgl_port.html) walks through a complete application: flush callback, flush completion, pointer input device and the headless variant.

## Example

[`examples/lvgl_sim_sdl`](examples/lvgl_sim_sdl) renders an LVGL screen into the panel, opens a preview window and runs a console command interface (`touch`, `frame`, `quit`, ...) that a pytest uses to click a button and compare frames against a golden image. See the [example README](examples/lvgl_sim_sdl/README.md) for how to build, run and test it.

## Notes and Limitations

- The component only supports the ESP-IDF host (`linux`) target. For a real display controller, use an `esp_lcd_*` panel driver.
- Supported framebuffer formats: RGB565 (little/big endian), RGB888, BGR888 and BGRA8888; see the [pixel format table](https://espressif.github.io/idf-extra-components/latest/esp_lcd_sim/index.html#pixel-formats).
- The panel stores the pixels exactly as the GUI submits them: rotation, gap, mirroring and color inversion are not applied.
- The API is split by thread: SDL and lifecycle calls run on a single GUI thread; touch injection and PNG export are thread-safe. See [Thread Safety and Lifecycle](https://espressif.github.io/idf-extra-components/latest/esp_lcd_sim/threading.html).
- A preview window needs a display; without one the panel falls back to framebuffer-only and can still export PNGs, which is what CI runs use.
