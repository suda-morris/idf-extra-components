# Porting an LVGL Application

This page walks through a complete LVGL 9 port on the simulated panel. It follows the runnable `examples/lvgl_sim_sdl` (in the component repository) application; the snippets below are taken from it.

The guiding principle: **the GUI port is written exactly as it would be for a real LCD.** It only calls the generic `esp_lcd` panel API and does not know whether the panel is the SDL sim panel or a display controller. Switching between hardware and simulation is a one-line change in the board initialization.

## Tick and Delay

LVGL needs a time source and a delay function. On a chip these come from a hardware timer and a busy wait; on the host, use the monotonic `esp_timer` and a real sleep so the task yields:

```c
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static uint32_t example_tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void example_delay_cb(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

// in app_main:
lv_init();
lv_tick_set_cb(example_tick_cb);
lv_delay_set_cb(example_delay_cb);
```

## Display and Flush Callback

Create the panel as shown in [Getting Started](index.md). One porting note: the sim panel has no hardware to program, so `esp_lcd_panel_reset()`, `esp_lcd_panel_mirror()`, `esp_lcd_panel_swap_xy()`, `esp_lcd_panel_set_gap()`, `esp_lcd_panel_invert_color()` and `esp_lcd_panel_disp_on_off()` are accepted as no-ops returning `ESP_OK` — a port that wraps them in `ESP_ERROR_CHECK()` runs unchanged. The ops that would change the picture on real hardware log a warning, because the preview keeps showing the framebuffer as drawn. `esp_lcd_panel_init()` is supported and clears the framebuffer to black.

Then register an LVGL display whose color format matches the panel's `color_format`. LVGL's `LV_COLOR_FORMAT_RGB888` stores pixels as B, G, R in memory, which is `ESP_COLOR_FOURCC_BGR24`:

```c
lv_display_t *display = lv_display_create(240, 320);
lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB888);   // matches ESP_COLOR_FOURCC_BGR24
lv_display_set_user_data(display, panel);
lv_display_set_flush_cb(display, example_lvgl_flush_cb);

static uint8_t draw_buf[240 * 40 * sizeof(lv_color_t)];
lv_display_set_buffers(display, draw_buf, NULL, sizeof(draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
```

The flush callback passes the draw buffer to the panel like it would to a physical LCD driver. Mind the coordinate convention: LVGL areas are inclusive, `esp_lcd` expects an exclusive right/bottom:

```c
static void example_lvgl_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(display);
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
}
```

## Flush Completion

A real transfer-based driver (SPI, I80, RGB with bounce buffers) reports the end of a color transfer asynchronously, and the LVGL port calls `lv_display_flush_ready()` from that callback. The sim panel provides the same hook: `on_flush_done` fires after each `esp_lcd_panel_draw_bitmap()` has copied its pixels into the framebuffer.

```c
static void example_on_flush_done(esp_lcd_panel_handle_t panel, void *user_ctx)
{
    lv_display_flush_ready((lv_display_t *)user_ctx);
}

ESP_ERROR_CHECK(esp_lcd_sim_register_event_callbacks(panel,
        &(esp_lcd_sim_event_callbacks_t) { .on_flush_done = example_on_flush_done },
        display));
```

Two differences from hardware are worth knowing:

- The copy is synchronous, so the callback fires **inside** the draw call, on the same thread. It runs outside the panel lock, so it may block (for example on LVGL) without holding up a concurrent screenshot.
- A partial refresh flushes several areas, so the callback fires once per draw call, not once per frame.

The callback must not call back into the drawing functions of the same panel. Register the callbacks before any thread starts drawing.

## Pointer Input Device

The host mouse plays the finger. Register a pointer indev whose read callback polls `esp_lcd_sim_input_get()`:

```c
static void example_mouse_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    esp_lcd_panel_handle_t panel = lv_indev_get_user_data(indev);
    esp_lcd_sim_input_data_t mouse = {0};
    if (esp_lcd_sim_input_get(panel, &mouse) == ESP_OK) {
        data->point.x = mouse.x;
        data->point.y = mouse.y;
        data->state = mouse.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    }
}

lv_indev_t *mouse_indev = lv_indev_create();
lv_indev_set_type(mouse_indev, LV_INDEV_TYPE_POINTER);
lv_indev_set_display(mouse_indev, display);
lv_indev_set_user_data(mouse_indev, panel);
lv_indev_set_read_cb(mouse_indev, example_mouse_read_cb);
```

The same read callback also works headless: when a test injects a touch with `esp_lcd_sim_input_inject_touch()`, `input_get()` reports the injected state instead of the mouse. See [Touch Input](input.md).

## The GUI Loop

Build your UI, force a first refresh, then run the loop from [Getting Started](index.md#run-the-preview-loop). The complete loop of the example handles both the windowed and the headless path:

```c
example_create_ui();
lv_refr_now(display);   // render the first frame now, don't wait for a timer tick

bool quit = false;      // set by the console 'quit' command, see the console page
while (!quit && (!with_window || esp_lcd_sim_window_is_open(panel))) {
    ESP_ERROR_CHECK(esp_lcd_sim_pump_events());
    uint32_t idle_ms = lv_timer_handler();
    if (!with_window) {
        // headless the wait is a sleep no injection can interrupt: cap it
        idle_ms = idle_ms > 20 ? 20 : idle_ms;
    }
    esp_lcd_sim_wait_events(idle_ms);
}
```

`with_window` is the `esp_lcd_sim_window_is_open(panel)` check from creation time: with a window, the loop ends when the user closes it; headless there is no window to close, so only `quit` ends the loop. The idle call itself is the same in both modes — `esp_lcd_sim_wait_events()` waits for a preview event with a window and sleeps the timeout without one.

`app_main` is the GUI thread: it owns the panel, the SDL preview and LVGL. A console REPL or a test driver runs on another thread and only uses the thread-safe entry points — [Thread Safety and Lifecycle](threading.md) explains the split.

## Exporting a Frame

The generic `esp_lcd` interface has no concept of a "complete GUI frame": one LVGL refresh may consist of several partial flushes. Export the panel content only after the refresh you want is finished, for example after a synchronous `lv_refr_now()`:

```c
lv_refr_now(display);
ESP_ERROR_CHECK(esp_lcd_sim_screenshot_save_png(panel, "screenshot.png"));
```

See [Screenshots and Golden Images](screenshot.md) for the golden-image workflow and the determinism settings the example uses.

## Teardown

Stop the GUI loop and any other thread that uses the panel, then tear down in order:

```c
ESP_ERROR_CHECK(esp_lcd_sim_screenshot_save_png(panel, "screenshot.png"));  // optional final frame
ESP_ERROR_CHECK(esp_lcd_panel_del(panel));
ESP_ERROR_CHECK(esp_lcd_sim_deinit());
```

If a console REPL is running, stop it before deleting the panel (`esp_console_stop_repl()` waits for the REPL task to exit), so no command can run against a deleted panel. The [console page](console.md#application-skeleton) shows the full two-thread teardown.
