# Changelog

## 0.1.0

- Initial version
- `esp_lcd_new_panel_host_sdl()` creates an `esp_lcd` panel backed by SDL: `draw_bitmap()` / `draw_bitmap_2d()` calls are stored in an internal framebuffer
- `esp_lcd_host_config_t` selects resolution, color format, preview window, preview scaling and window title per panel
- Optional SDL preview window (`create_window`), refreshed by `esp_lcd_host_pump_events()`
- `esp_lcd_host_screenshot_save_png()` streams scanlines into a PNG file via libpng `png_write_row` (no extra full-frame RGB buffer)
- `esp_lcd_host_return_panel()` and `esp_lcd_host_return_buffers()` publish the latest frame to the SDL simulator
- `esp_lcd_host_get_target()` reports whether the application runs on the host (POSIX) target or on a chip
- The component manifest restricts the component to the `linux` target, it is a host simulation only
