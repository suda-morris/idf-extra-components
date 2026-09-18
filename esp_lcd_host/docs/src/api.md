# API Reference

## Header File

- [esp_lcd_host.h](https://github.com/espressif/idf-extra-components/blob/master/esp_lcd_host/include/esp_lcd_host.h)

The Doxygen documentation is generated from the header file during the documentation build.

## Functions

### Panel Creation

- `esp_lcd_new_panel_host_sdl()` — Create an `esp_lcd` panel backed by SDL.

### Simulation Control

- `esp_lcd_host_get_target()` — Report what the panel simulator is running on.
- `esp_lcd_host_return_panel()` — Publish the latest frame of the panel.
- `esp_lcd_host_return_buffers()` — Publish the panel framebuffer.
- `esp_lcd_host_pump_events()` — Pump the SDL event queue of the preview windows.

### Frame Export

- `esp_lcd_host_screenshot_save_png()` — Save the panel content as a PNG file.

## Types

### Configuration

- `esp_lcd_host_config_t` — Panel resolution, color format and preview window settings.

### Enumerations

- `esp_lcd_host_target_t` — Target the panel simulator is running on.

## Workaround

`esp_lcd` does not build for the ESP-IDF host (`linux`) target yet. The panel handle definition and the generic panel operations are therefore taken from `esp_lcd` when they are available and replicated otherwise, see `src/esp_lcd_host_types.h`. As soon as `esp_lcd` supports the host target, those fallbacks are dropped without any change to the API of this component.
