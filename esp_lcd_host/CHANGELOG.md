# Changelog

## 0.1.0

- Initial version
- `esp_lcd_new_panel_host_sdl()` creates an `esp_lcd` panel backed by SDL: `draw_bitmap()` / `draw_bitmap_2d()` calls are stored in an internal framebuffer
- `esp_lcd_host_config_t` selects resolution, color format, preview window, preview scaling and window title per panel
- SDL3 is vendored as the `esp_lcd_host/SDL` submodule and built by `port/sdl/CMakeLists.txt` with the CMake project of SDL itself: `add_subdirectory()` reuses the SDL source lists and generates `SDL_build_config.h`. The port keeps video, events, threads, timer and filesystem with the software renderer, the X11 and Wayland drivers so the preview window opens on a desktop (both probed and enabled by default, both can be turned off), the KMSDRM driver when `libdrm`, `gbm` and EGL are present and the dummy and offscreen drivers everywhere else. Audio, camera, joystick, haptic, hidapi, sensor, power, dialog, tray, GPU and the GPU/board video drivers are disabled, and the optional dependencies SDL probes for (Fribidi, libthai, D-Bus, IBus, libudev, liburing) are turned off so the build does not depend on the build machine
- The component depends on `espressif/libpng` for `esp_lcd_host_screenshot_save_png()` and on ESP-IDF, and `port/sdl/CMakeLists.txt` is the only file to review when the SDL submodule is updated; the source lists and the build configuration come from SDL
- `idf_component_register()` requires `esp_lcd` again, the host fallback of the panel interface stays as a workaround until `esp_lcd` builds for the `linux` target
- Optional SDL preview window (`create_window`), refreshed by `esp_lcd_host_pump_events()`
- `esp_lcd_host_screenshot_save_png()` streams scanlines into a PNG file via libpng `png_write_row` (no extra full-frame RGB buffer)
- `esp_lcd_host_return_panel()` and `esp_lcd_host_return_buffers()` publish the latest frame to the SDL backend
- `esp_lcd_host_get_target()` reports whether the application runs on the host (POSIX) target or on a chip
- The component manifest restricts the component to the `linux` target, it is a host simulation only. `linux` is the name ESP-IDF gives the host target on every operating system, so the component builds on Linux, macOS and Windows: `port/sdl/CMakeLists.txt` reads the platform from `CMAKE_SYSTEM_NAME` and lets SDL select the window driver (X11, Wayland or KMSDRM on Linux, Cocoa on macOS, Win32 on Windows), the thread back end and the platform sources. Only the Linux desktop packages are probed, and `src/esp_lcd_host.c` no longer depends on `<sys/param.h>`
- The X11 probe of `port/sdl/CMakeLists.txt` requires XInput2 together with XFixes, because SDL stops the configuration when XFixes is enabled and XInput2 is not; a machine with an incomplete set of X11 packages now loses the X11 driver instead of failing to configure
- The example falls back to a framebuffer only panel when the preview window cannot be created, so it also runs on a headless machine
