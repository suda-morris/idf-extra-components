# Touch Input

The simulated panel reports a single touch contact, the way a capacitive touch controller reports one finger. There are two sources for it, and the GUI cannot tell them apart:

1. the **host mouse** in the preview window, for interactive development;
2. **programmatic injection**, for automated tests — with or without a window.

Both are read through the same function, `esp_lcd_sim_input_get()`, which is what your GUI's input device polls (see the [LVGL port](lvgl_port.md#pointer-input-device)).

## The Mouse as Finger

While the pointer is over the **screen area** of the preview window — the LCD cut-out when `show_board_frame` is set, the whole window otherwise — the panel maps it into panel pixels and reports the left button as the touch contact:

```c
esp_lcd_sim_input_data_t mouse = {0};
ESP_ERROR_CHECK(esp_lcd_sim_input_get(panel, &mouse));
// mouse.x, mouse.y: position in panel pixels
// mouse.pressed:    left button held while on the screen area
```

Details worth knowing:

- **Off the glass** (on the board frame, on another window) the touch releases, but the last on-screen position is kept. A GUI library therefore still gets hover information, and hover styles keep working when the pointer leaves the screen. Before the first visit the position is `(0, 0)`.
- The state is SDL's knowledge **as of the last `esp_lcd_sim_pump_events()` call**. Poll it in the same loop that pumps — an LVGL input device read callback is a natural fit.
- A panel without a preview window reports released at the last sampled position, `(0, 0)` if the pointer never was there — unless a touch is injected, see below.
- `esp_lcd_sim_input_get()` belongs to the GUI thread (it samples SDL). The injected override it may report is synchronized with the injecting thread, see [Thread Safety and Lifecycle](threading.md).

## Programmatic Injection

For automated tests the pointer can be driven from code instead of the mouse:

```c
esp_err_t esp_lcd_sim_input_inject_touch(esp_lcd_panel_handle_t panel, int x, int y, bool pressed);
esp_err_t esp_lcd_sim_input_inject_reset(esp_lcd_panel_handle_t panel);
```

`esp_lcd_sim_input_inject_touch()` sets the state that `esp_lcd_sim_input_get()` reports for this panel, replacing the real mouse. The state is **sticky**: it is kept until the next inject call or `esp_lcd_sim_input_inject_reset()`, so a polling input device sees a stable press or release across as many reads as it needs.

This is a *state sample*, not an event queue. To script a click, inject pressed, let the GUI run, then inject released:

```c
esp_lcd_sim_input_inject_touch(panel, 120, 215, true);   // finger down
run_gui_for_a_while();                                   // e.g. vTaskDelay(100 ms)
esp_lcd_sim_input_inject_touch(panel, 120, 215, false);  // lift -> click
run_gui_for_a_while();
```

A swipe is a sequence of pressed moves followed by a release:

```c
esp_lcd_sim_input_inject_touch(panel, 120, 280, true);
for (int y = 280; y >= 80; y -= 20) {
    esp_lcd_sim_input_inject_touch(panel, 120, y, true);
    run_gui_for_a_while();
}
esp_lcd_sim_input_inject_touch(panel, 120, 80, false);
```

"Let the GUI run" matters: the component does not know your GUI library and cannot drive it. The GUI thread must sample the new state and finish its gesture processing (click detection, scroll momentum, animations) before you export a frame or inject the next state.

### Why Injection Is the Key to Host Testing

Injection works **with or without a preview window**, which is what lets a headless test drive the GUI: inject a touch, run the GUI, export a PNG and compare. The whole loop runs on the development machine or in CI, no display and no hardware involved:

```text
inject touch --> GUI thread samples it --> GUI reacts --> export PNG --> compare
```

While injection is active the real mouse is ignored for that panel; `esp_lcd_sim_input_inject_reset()` hands control back to it. The injected pointer is not drawn into the preview and does not move the real cursor.

Mind the difference between the two ways to end a press: `esp_lcd_sim_input_inject_touch(panel, x, y, false)` is a **lifted finger at (x, y)** — the override stays installed, so the GUI keeps reading the injected released state and the mouse stays ignored. `esp_lcd_sim_input_inject_reset()` **removes the override**, and the next read samples the real mouse again. A scripted click ends with the first; a test that hands the panel back to an interactive user ends with the second.

Both functions are **thread-safe**: a command thread (for example a console REPL task) may call them while the GUI thread draws. The [console command interface](console.md) is built on exactly this.

## Choosing the Source

| Situation | Source |
|---|---|
| Developing the UI interactively | mouse in the preview window |
| Automated test, same process | `esp_lcd_sim_input_inject_touch()` from the test driver |
| Automated test, external process (pytest) | [console commands](console.md) over stdin |
| Back to interactive after a test | `esp_lcd_sim_input_inject_reset()` (a release injection is not enough: it keeps the override) |
