# SDL Host Panel Programming Guide

`esp_lcd_host` provides a virtual [`esp_lcd`](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/peripherals/lcd.html) panel that renders into an [SDL](https://www.libsdl.org/) window, so a GUI can be developed on a development machine without attaching a display.

The panel implements the generic `esp_lcd_panel_t` interface, the same interface that real LCD drivers, such as `esp_lcd_ili9341` or `esp_lcd_st7701`, implement. Because of that, a GUI port can be switched between a real display and the simulated one by replacing only the LCD initialization code.

## Overview

The component solves three common problems of GUI development on embedded hardware:

- **No display needed** — the GUI output is rendered into an SDL window on the host, and the same `idf.py build` builds the application.
- **GUI code stays portable** — the panel speaks `esp_lcd`, so the LVGL flush callback and the display driver registration do not change.
- **Frames can be verified automatically** — the panel content can be saved as a PNG file and compared against a golden image in CI.

The panel keeps a full-size copy of the pixels submitted by the GUI. Rotation, gap, mirroring and color inversion are not applied to that copy, in the same way that they are not applied to the preview window.

## API Usage Workflow

The diagram below shows the lifecycle of an application built on the SDL host panel. The **panel setup** stage replaces the LCD initialization of the board, **GUI registration** is the part that stays unchanged between a real display and the simulation, and the **frame export** stage makes the rendered content visible and testable. The dotted edge marks the frame publishing calls, which are only needed when the simulator runs in a separate process.

```mermaid
%%{init: {"theme": "base", "themeVariables": {"lineColor": "#7b8794"}}}%%
flowchart TD
    start(["Start"]):::entry

    subgraph SETUP["1 · Panel Setup"]
        direction TB
        cfg["esp_lcd_host_config_t<br/>width, height, color_format,<br/>preview window, scale"]:::setup
        del["esp_lcd_panel_del()"]:::cleanup
        create["esp_lcd_new_panel_host_sdl()"]:::setup
        init["esp_lcd_panel_reset()<br/>esp_lcd_panel_init()"]:::setup
    end

    subgraph GUI["2 · GUI Registration"]
        direction TB
        lvgl["Register the panel with<br/>the GUI library"]:::gui
        flush["GUI flush callback<br/>esp_lcd_panel_draw_bitmap()"]:::gui
        pump["esp_lcd_host_pump_events()<br/>refresh the preview window"]:::gui
    end

    subgraph EXPORT["3 · Use the Rendered Frame"]
        direction TB
        png["esp_lcd_host_screenshot_save_png()"]:::export
        ext["esp_lcd_host_return_panel()<br/>esp_lcd_host_return_buffers()"]:::export
        inspect["Inspect the window,<br/>the PNG file, or compare<br/>against a golden image"]:::validate
    end

    subgraph CLEANUP["4 · Cleanup"]
        direction TB
        destroy["esp_lcd_panel_del()<br/>closes the preview window"]:::cleanup
        end_node(["Done"]):::entry
    end

    start --> cfg --> create --> init --> lvgl --> flush
    lvgl --> pump
    flush --> pump
    pump -- "another frame" --> flush
    pump -- "frame is complete" --> png
    pump -.-> ext
    png --> inspect
    ext --> inspect
    inspect --> destroy --> end_node
    destroy -.-> del
```

## Prerequisites

The component depends on the [`georgik/sdl`](https://components.espressif.com/components/georgik/sdl) component, which provides SDL3 together with its board abstraction layer `georgik/sdl_bsp`. The abstraction layer would otherwise initialize the display and touch driver of a board, which would conflict with the panel of this component, so select **No board (SDL only)** under *ESP-BSP SDL Configuration* in `menuconfig`:

```
CONFIG_SDL_BSP_NO_BOARD=y
```

With that option set the abstraction layer keeps SDL running and leaves the display to the caller, which is exactly the panel created by `esp_lcd_new_panel_host_sdl()`.

## Configuration

The panel is configured with `esp_lcd_host_config_t` at initialization time:

| Field | Description |
| --- | --- |
| `width`, `height` | Panel resolution. `draw_bitmap()` calls are clipped to this rectangle. |
| `color_format` | Pixel layout the GUI submits, as an `esp_color_fourcc_t`, for example `ESP_COLOR_FOURCC_BGR24` for LVGL RGB888 or `ESP_COLOR_FOURCC_RGB16` for RGB565. |
| `create_window` | Opens an SDL preview window. Leave it `false` on machines without a display, for example in CI. |
| `use_renderer` | Lets SDL scale the preview instead of requiring the window size to match the panel size. |
| `scale` | Integer upscale factor for the preview window and the SDL renderer output. |
| `window_title` | Preview window title, a default is used when `NULL`. |

The configuration is stored per panel, so several panels with different resolutions and color formats can coexist, and an export always uses the geometry of its own panel.

## Color formats

The fourcc values follow the `esp_color_fourcc_t` encoding of ESP-IDF and are packed little-endian: the first character is the least significant byte.

| FourCC | Description |
| --- | --- |
| `RGB16` | RGB565, little-endian `uint16_t` |
| `RGB16_BE` | RGB565, big-endian |
| `RGB24` | RGB888, bytes in R, G, B order |
| `BGR24` | RGB888, bytes in B, G, R order (LVGL RGB888 and most ESP-IDF RGB888 paths) |
| `ARGB8888`, `ABGR8888`, `RGBA8888`, `BGRA8888` | 32bpp with alpha, in the byte order given by the name |

PNG export always produces 8 bits per channel. For 32bpp formats an all-zero alpha channel is treated as opaque, so an XRGB8888 frame does not end up fully transparent.

## Frame export

- `esp_lcd_host_screenshot_save_png()` writes the panel content as a PNG file. Colors are converted scanline by scanline and handed to libpng with `png_write_row()`, so no extra full-frame RGB buffer is allocated.
- `esp_lcd_host_return_panel()` and `esp_lcd_host_return_buffers()` hand the latest frame to the SDL simulator when it runs as a separate process.

Export and publish the panel content only after the GUI finished the frame you want, because as with any `esp_lcd` panel a frame can consist of several partial flushes.

## Preview window

The preview window shows the panel framebuffer directly, so submitting a frame copies nothing. `esp_lcd_host_pump_events()` refreshes the window surface, but only after the GUI has drawn something, because `SDL_UpdateWindowSurface()` is synchronous and would otherwise steal time from the GUI.

Applications must call `esp_lcd_host_pump_events()` periodically while a preview window is open. On the ESP-IDF host target the LVGL timer handler is a convenient place:

```c
static void example_lv_timer_cb(lv_timer_t *timer)
{
    lv_tick_inc(EXAMPLE_LV_TICK_PERIOD_MS);
    lv_timer_handler();
    ESP_ERROR_CHECK(esp_lcd_host_pump_events());
}
```

## Target detection

`esp_lcd_host_get_target()` reports whether the application runs on the ESP-IDF host (`linux`) target or on a chip. This is useful for code that wants to adapt the simulation, for example to slow down animation ticks so the frames stay readable:

```c
esp_lcd_host_target_t target = ESP_LCD_HOST_TARGET_ESP32;
ESP_ERROR_CHECK(esp_lcd_host_get_target(panel, &target));
if (target == ESP_LCD_HOST_TARGET_POSIX) {
    // Running in the simulation, use a slower animation period.
}
```

## Limitations

- The panel never blocks: `draw_bitmap()` copies the pixels and returns. A GUI that relies on the transfer-done callback of a real panel should not expect an asynchronous notification.
- The exported image shows the pixels submitted by the GUI, before any rotation, gap, mirroring or color inversion is applied.
- 16bpp formats are exported through the RGB565 to RGB888 expansion, which loses precision.
- The preview window requires a display. Without one, SDL window creation fails and the panel continues to work as a framebuffer only.
