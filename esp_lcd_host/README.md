# Simulate a Display: SDL Backed Virtual `esp_lcd` Panel

[![Component Registry](https://components.espressif.com/components/espressif/esp_lcd_host/badge.svg)](https://components.espressif.com/components/espressif/esp_lcd_host)

`esp_lcd_host` is a virtual [`esp_lcd`](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/lcd.html) panel driver that renders into an [SDL](https://www.libsdl.org/) window on a development machine. When a GUI needs a display but wiring up a screen is inconvenient, the GUI port can be built against this panel instead of a real display controller:

- no LCD, no ribbon cable, no board: the same `idf.py build` builds the GUI against the simulated panel,
- the GUI code stays untouched, because the panel implements the generic `esp_lcd_panel_t` interface used by every LCD driver,
- the rendered frame can be exported as a PNG file and compared against a golden image in CI.

The component only builds for the ESP-IDF host (`linux`) target, where the SDL window runs in the same process as the application. The manifest of the component declares that target, so the component manager rejects the dependency for a real chip instead of failing later in the build.

## Installation

Add the component to your project's `idf_component.yml`:

```yaml
dependencies:
  espressif/esp_lcd_host: "^0.1.0"
```

Then run `idf.py reconfigure` or build the project. The SDL and libpng dependencies are resolved by the IDF component manager.

> The component requires the `linux` target, ESP-IDF `>= 6.0.0` and the [`georgik/sdl`](https://components.espressif.com/components/georgik/sdl) component. Taking the published `georgik/sdl` also pulls in `georgik/sdl_bsp`, the board abstraction layer of the SDL ecosystem; select **No board (SDL only)** in `menuconfig` under *ESP-BSP SDL Configuration* so that no board BSP is initialized and the panel of this component is used instead. The `examples/lvgl_host_sdl/sdkconfig.defaults` file does that already.

## Quick start

Create the panel with the same resolution and pixel format as the framebuffer produced by your GUI library, initialize it, and pass the panel handle to the GUI display driver:

```c
#include "esp_lcd_host.h"

esp_lcd_host_config_t config = {
    .width = 240,
    .height = 240,
    .color_format = ESP_COLOR_FOURCC_BGR24,  // LVGL RGB888 in memory
    .create_window = true,                   // set to false on a machine without a display
    .use_renderer = true,                    // let SDL scale the preview window
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

[`examples/lvgl_host_sdl`](examples/lvgl_host_sdl) renders an LVGL screen into the SDL panel, opens a preview window, writes a PNG and a PPM file, and compares the frame against a golden image from a pytest script:

```bash
cd examples/lvgl_host_sdl
idf.py set-target linux
idf.py build
./build/lvgl_host_sdl.elf
```

## Notes and limitations

- The component only supports the `linux` target. There is no driver for a real display controller, use an `esp_lcd_*` panel driver for that.
- The panel stores the pixels submitted by the GUI, so rotation, gap, mirroring and color inversion are not applied to the exported image. The same is true for the preview window.
- A frame may consist of several partial flushes. Export or publish the panel content only after the refresh you want is finished (for example after a synchronous `lv_refr_now()`).
- `create_window` needs a display in the environment where the application runs. Without one, SDL window creation fails and the panel keeps working as a framebuffer only.
- Preview scaling is handled by SDL. The exported PNG always uses the panel resolution.
