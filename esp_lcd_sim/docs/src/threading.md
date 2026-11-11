# Thread Safety and Lifecycle

The API is split into two groups so a GUI can render on one thread while a command interface — for example an `esp_console` REPL task — drives input and exports frames from another. This page explains the split, the guarantees behind it, and the lifecycle rules that keep teardown safe.

## The Two API Groups

| GUI thread only | Any thread |
|---|---|
| `esp_lcd_new_panel_sim_sdl()` | `esp_lcd_sim_input_inject_touch()` |
| `esp_lcd_panel_init()` | `esp_lcd_sim_input_inject_reset()` |
| `esp_lcd_panel_draw_bitmap()` / `draw_bitmap_2d()` | `esp_lcd_sim_screenshot_save_png()` |
| `esp_lcd_sim_input_get()` | |
| `esp_lcd_sim_pump_events()` | |
| `esp_lcd_sim_wait_events()` | |
| `esp_lcd_sim_window_is_open()` | |
| `esp_lcd_panel_del()` | |
| `esp_lcd_sim_deinit()` | |

The **GUI thread** is the thread that creates the panels and draws into them. Everything that touches SDL or the panel lifecycle runs there: SDL windows and the SDL event queue are not thread-safe, and the preview's dirty-region bookkeeping lives on this thread too. Create the panels, draw into them and pump the preview from one thread.

The **any-thread** group never touches SDL. A per-panel mutex guards the framebuffer pixels and the touch override, so a command thread can inject a touch or snapshot a frame while the GUI thread draws, without tearing the image.

## What the Mutex Guarantees — and What It Does Not

Guaranteed:

- A screenshot is one consistent frame: the framebuffer is copied under the lock, and the (slow) PNG encoding runs outside it, so an export never stalls the GUI and never captures a half-drawn frame.
- An injected touch state is published atomically: the GUI thread's `esp_lcd_sim_input_get()` sees either the old or the new state, never a mix.
- `esp_lcd_panel_draw_bitmap()` copies pixels under the same lock, so drawing and snapshotting serialize against each other.

Not guaranteed:

- **Ordering between threads.** If several threads inject touches, the arrival order is undefined; the last write wins. Script input from a single thread (the REPL task does).
- **Lifetime protection.** The mutex serializes concurrent use; it does not make a deleted panel safe to call. Stop every thread that uses the panel before `esp_lcd_panel_del()`.

## Callback Context

`on_flush_done` runs synchronously on the GUI thread that called the triggering draw function, **outside** the panel mutex:

- It may block (for example waiting on LVGL) without holding up a concurrent screenshot.
- It must not call back into the drawing functions of the same panel.
- Register the callbacks before any thread starts drawing; the registration itself is safe at any time while the panel is alive.

## Lifecycle Rules

1. **Create** the panel and register the event callbacks on the GUI thread, before other threads start.
2. **Run** the GUI loop on the GUI thread; command threads only use the any-thread group (directly or through the [console commands](console.md)).
3. **Stop the command threads first.** With a console REPL, `esp_console_stop_repl()` waits for the REPL task to exit and frees it, so no command can run against a deleted panel afterwards.
4. **Stop the GUI loop**, then `esp_lcd_panel_del()` for every panel.
5. **`esp_lcd_sim_deinit()` last**, once, before the process exits (or before using SDL for something else). It returns `ESP_ERR_INVALID_STATE` while a panel with a live preview backend still exists.

`esp_lcd_sim_deinit()` quits SDL as a whole, not just the video subsystem: SDL subsystem refcounts are owned by SDL, and `SDL_Quit()` is the only teardown that reliably releases them. Do not call it while another part of the process still uses SDL.

## Process-Wide SDL

SDL is a process-global library, and the component treats it that way:

- The event queue is shared. `esp_lcd_sim_pump_events()` drains the **whole** queue (not only the events it acts on), so that `esp_lcd_sim_wait_events()` can block until new input arrives. An application that also uses SDL must coordinate its own event loop with this one — do not run a second, concurrent loop.
- Pointer input does not come from the queue: `esp_lcd_sim_input_get()` samples SDL's cached mouse state directly, so discarding mouse events loses nothing.
- The video subsystem is initialized when the first preview window is created and kept until `esp_lcd_sim_deinit()`.
