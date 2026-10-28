# raylib-fireworks

A fireworks show on a 320x240 ST7789 SPI panel: rockets rise from behind a
generated city skyline and burst into coloured particles under a twinkling
night sky. The port has no input, so the show plays itself; every frame is
immediate-mode raylib with no assets.

On an ESP32-S3 at 240 MHz this runs at around 15-20 fps. The scene is written
with the render cost of a software rasteriser in mind:

- The sky is 60 flat-colour bands, not one gradient quad: a `DrawRectangle`
  with a single colour fills several times faster than a quad whose vertex
  colours interpolate per pixel.
- Small dots are 2-3 px rectangles, not `DrawCircle()`: a circle is emitted as
  36 triangles, each through the polygon clipper, which costs far more than
  the few pixels it covers.
- The simulation runs on a fixed-step accumulator (1/30 s, up to 8 catch-up
  steps), so the show keeps pace with wall time whatever the frame rate does.

## What it shows

- Initialising an `esp_lcd` panel by hand, then handing it to raylib through
  `esp_raylib_set_display()`
- Sending frames in 48-line bands so the SPI DMA transfer stays small
- The `flush_cb` contract: `esp_lcd_panel_draw_bitmap()` queues its DMA and
  returns early, so the example waits for `on_color_trans_done` before
  returning and the component may reuse its staging buffer
- Immediate-mode drawing: flat-colour fills, lines, HSV colour, `Fade()`,
  `GetTime()`-driven motion and a pooled particle system

## Build and run

```bash
idf.py set-target esp32s3
idf.py build flash monitor
```

The panel pins at the top of `main/fireworks_main.c` are for an
ESP32-S3-BOX style board. Change them, `LCD_H_RES` and `LCD_V_RES` to match
your hardware; nothing else in the example is board specific.

If the image shows up mirrored or upside down, flip `display.invert` or swap
the `esp_lcd_panel_swap_xy()` / `esp_lcd_panel_mirror()` calls in
`display_init()`.
