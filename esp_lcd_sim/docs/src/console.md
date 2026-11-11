# Console Command Interface

The [injection API](input.md) lets code in the same process drive the GUI. The console command interface goes one step further: it exposes the injection and the frame export as a fixed set of [`esp_console`](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/console.html) commands, so an **external** process — a pytest script — drives the GUI by writing command lines to the application's stdin.

The commands are provided by the component itself, not by the application: their names and arguments are part of the public API of `esp_lcd_sim`. The application only creates a REPL and registers the set; it may register its own, unrelated commands on the same REPL.

## Enable and Register

Enable the command set in `sdkconfig.defaults` (it pulls in the `console` component):

```text
CONFIG_ESP_LCD_SIM_CONSOLE=y
```

Then create a background REPL and register the set on it:

```c
#include "esp_console.h"
#include "esp_lcd_sim.h"

bool quit = false;   // must outlive the registration and the REPL task
const esp_lcd_sim_console_config_t console_config = {
    .panel = panel,
    .touch_settle_ms = 100,     // GUI run time after each touch, 0 selects 100 ms
    .quit_requested = &quit,    // 'quit' sets this flag; NULL makes 'quit' a no-op
};

esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
repl_config.prompt = "my_gui>";
esp_console_repl_t *repl = NULL;
ESP_ERROR_CHECK(esp_console_new_repl_stdio(&repl_config, &repl));  // spawns the REPL task
ESP_ERROR_CHECK(esp_lcd_sim_console_register(&console_config));
ESP_ERROR_CHECK(esp_console_start_repl(repl));
```

Notes:

- The config structure is copied; only the `quit_requested` pointer is kept, so the flag it points to must outlive the registration.
- The command set drives a **single panel**; register it once after the REPL is created. A second registration returns `ESP_ERR_INVALID_STATE`.
- With `CONFIG_ESP_LCD_SIM_CONSOLE` disabled, `esp_lcd_sim_console_register()` returns `ESP_ERR_NOT_SUPPORTED` and the panel itself is unaffected — the same application code builds with and without the command set.

## Command Reference

| Command | Effect |
|---|---|
| `touch <x> <y> <0\|1>` | inject a pointer state at panel pixel (x, y): 1 = press, 0 = release; then sleep `touch_settle_ms` |
| `reset` | stop injecting, return to the real pointer |
| `frame <path>` | export the current frame as a PNG to the exact path |
| `wait <ms>` | sleep while the GUI runs without input (capped at 10000 ms) |
| `quit` | set `*quit_requested` so the application loop can stop |
| `help` | list the commands (built-in `esp_console` help) |

Every command only calls the thread-safe panel entry points (`inject_touch`, `inject_reset`, `screenshot_save_png`) and never SDL, so the commands run on the **REPL task** while the GUI renders on its own thread. See [Thread Safety and Lifecycle](threading.md).

### Why `touch` and `wait` Sleep

The component does not know the GUI library and cannot drive it: after an injection, the independent GUI thread must sample the new state and finish its gesture processing before the next command (for example a `frame`) runs. The commands therefore give the GUI wall time: `touch` sleeps `touch_settle_ms` afterwards, `wait` sleeps the requested time.

Tune `touch_settle_ms` to your GUI: it must cover the input device read period plus the gesture processing (click detection, animations you want settled). 100 ms is a comfortable default for LVGL with theme transitions disabled; raise it if your GUI animates on press.

## Application Skeleton

The application runs two threads, the way a real application does: `app_main` is the GUI thread (LVGL + SDL preview), the REPL task runs the commands. The `examples/lvgl_sim_sdl` example in the component repository implements exactly this; the skeleton is:

```c
void app_main(void)
{
    // 1. Create the panel and port the GUI (see the LVGL port page).
    //    bool with_window = esp_lcd_sim_window_is_open(panel);
    //    This thread is the GUI thread from here on.

    // 2. Start the background REPL and register the command set,
    //    plus any application commands of your own.
    bool quit = false;
    // ... esp_console_new_repl_stdio + esp_lcd_sim_console_register + start_repl ...
    ESP_ERROR_CHECK(esp_console_cmd_register(&my_own_cmd));   // coexists fine

    // 3. GUI loop: render until 'quit' or the window is closed.
    while (!quit && (!with_window || esp_lcd_sim_window_is_open(panel))) {
        ESP_ERROR_CHECK(esp_lcd_sim_pump_events());
        uint32_t idle_ms = lv_timer_handler();
        // esp_lcd_sim_wait_events() idles in both modes; headless, bound
        // idle_ms so an injected touch is sampled promptly
    }

    // 4. Teardown: final frame, then stop the REPL task BEFORE deleting
    //    the panel, so no command can run against a deleted panel.
    ESP_ERROR_CHECK(esp_lcd_sim_screenshot_save_png(panel, "screenshot.png"));
    ESP_ERROR_CHECK(esp_console_stop_repl(repl));   // waits for the task to exit
    ESP_ERROR_CHECK(esp_lcd_panel_del(panel));
    ESP_ERROR_CHECK(esp_lcd_sim_deinit());
}
```

> **Linux target note:** keep `CONFIG_VFS_SUPPORT_IO` disabled when the application calls `esp_console_stop_repl()` (the example disables it in its `sdkconfig.defaults`). The linux VFS layer interposes `read`/`write`/`select` but not `eventfd()`, so the eventfd the console uses to interrupt its REPL task stays a raw kernel fd that `esp_vfs_write()` rejects with `EBADF`, and stopping the REPL fails. This is an ESP-IDF limitation, not of this component.

## Driving the GUI from pytest

With the REPL on stdin, a [pytest-embedded](https://docs.espressif.com/projects/pytest-embedded/en/latest/) test writes command lines and reads the application's log output. Two rules make this robust:

1. **Wait for the prompt before the first write.** `esp_console_new_repl_stdio()` probes the terminal for up to 500 ms and discards stdin bytes that are not a probe reply; a command written earlier would be swallowed. `dut.expect_exact('my_gui>')` first.
2. **Synchronize on the log, not on sleeps.** Each command logs what it did (`injected touch (120, 215) pressed`, `Saved 240x320 PNG image ... to 'frame.png'`); expect those lines before writing the next command. The `frame` log also tells the test the exact path that was written.

A click test then looks like this (abridged from the example test (`examples/lvgl_sim_sdl/pytest_lvgl_sim_sdl.py`)):

```python
def test_click(dut):
    dut.expect_exact('my_gui>')          # REPL ready, terminal probe done

    def frame(name):
        dut.write(f'frame {name}')
        path = dut.expect(SAVED_PNG_PATTERN).group('path').decode()
        return read_png(Path(path))

    before = frame('before.png')
    dut.write('touch 120 215 1')         # press the button
    dut.expect(r'injected touch \(120, 215\) pressed')
    dut.write('touch 120 215 0')         # release -> click
    dut.expect(r'injected touch \(120, 215\) released')
    after = frame('after.png')

    dut.write('quit')
    dut.expect_exact('example done.')
    assert before.pixels != after.pixels  # the GUI reacted
    # ... and assert that only the expected region changed
```

Because application and test share the file system, the test reads the PNG directly: no serial transfer, no base64, no intermediate image format. Combine this with the [golden image workflow](screenshot.md#golden-image-workflow) for full visual regression: static frame against golden, interaction frames against each other.
