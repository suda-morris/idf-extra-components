# Screenshots and Golden Images

The panel framebuffer can be exported as a PNG file at any time. This is the basis of visual regression testing on the host: render a frame, export it, compare it against a committed golden image.

## Exporting a Frame

```c
esp_err_t esp_lcd_sim_screenshot_save_png(esp_lcd_panel_handle_t panel, const char *filepath);
```

```c
ESP_ERROR_CHECK(esp_lcd_sim_screenshot_save_png(panel, "screenshot.png"));
```

Properties of the export:

- **Exact path.** The function always writes the exact path it is given, overwriting an existing file. (The interactive [screenshot key](#the-screenshot-key) never overwrites — the two behaviors are deliberately different.)
- **Panel resolution.** The PNG uses the `width`, `height` and `color_format` given at creation time. Preview scaling, window resizing and the board frame never affect it.
- **One consistent frame.** The framebuffer is snapshotted under the panel lock, so the export is never torn even while the GUI thread keeps drawing. The PNG encoding runs outside the lock, so a concurrent export does not stall the GUI.
- **Any thread.** The function never touches SDL; a console REPL task or a test driver may call it while the GUI renders. See [Thread Safety and Lifecycle](threading.md).
- **Works headless.** No preview window is needed, which is what the CI runs use.

The PNG is RGB, or RGBA when the format has an alpha channel that is actually used; an all-zero alpha (common with `ESP_COLOR_FOURCC_BGRA32` framebuffers) is treated as opaque so the image is not fully transparent.

## Export After a Complete Refresh

The generic `esp_lcd` interface has no concept of a complete GUI frame: one LVGL refresh may consist of several partial flushes, each one a `esp_lcd_panel_draw_bitmap()` call. Exporting in the middle of a refresh captures a half-updated frame.

Export only after the refresh you want is finished, for example after a synchronous `lv_refr_now()`:

```c
lv_refr_now(display);   // blocks until the whole screen is flushed
ESP_ERROR_CHECK(esp_lcd_sim_screenshot_save_png(panel, "frame.png"));
```

In an event-driven test, wait for the GUI to settle first — the [console interface](console.md) does this with a settle delay after each injected touch.

### Let the GUI Announce the Frame

When the export must happen on a frame boundary but is triggered from elsewhere, use the frame hook of your GUI library rather than a timing assumption. LVGL sends `LV_EVENT_REFR_READY` to the display once a refresh is complete:

```c
static void example_refr_ready_cb(lv_event_t *event)
{
    // GUI thread, right after a complete refresh: nothing is half-drawn here
    esp_lcd_sim_screenshot_save_png(panel, "frame.png");
}

lv_display_add_event_cb(display, example_refr_ready_cb, LV_EVENT_REFR_READY, NULL);
```

The panel itself offers no "frame finished" marker on purpose: it sees `draw_bitmap()` calls, not frames, so only the GUI library can say where one ends. A port that can call such a marker can export at the same point instead, and an exporter *waiting* for a marker would hang on a static screen that never redraws. The framebuffer snapshot is already atomic (see above); what the GUI hook adds is the frame semantics on top of it.

## The Screenshot Key

For interactive use, configure a key that saves a PNG without stopping the application:

```c
esp_lcd_sim_config_t config = {
    // ...
    .screenshot_key = 's',              // 0 disables the feature
    .screenshot_path = "screenshot.png", // NULL selects "screenshot.png"
};
```

Press `s` in the preview window and the current frame is written next to the configured name. An ASCII letter matches with or without Shift; a held-down key does not retrigger.

**Consecutive presses never overwrite.** The configured name is used while it is free; once it exists, presses write the first free numbered variant:

```text
screenshot.png
screenshot_001.png
screenshot_002.png
...
```

The number goes before the extension (a dot in a directory name does not count), and the directory is scanned on every press, so the numbering stays correct across application restarts. Up to 1000 variants are tried.

This makes the key convenient for exploratory work: press it whenever something looks interesting and collect the frames, without managing file names. Programmatic exports via `esp_lcd_sim_screenshot_save_png()` keep exact-path semantics instead, because a test wants to control where the frame lands.

## Golden Image Workflow

A golden-image test renders a known UI, exports the frame and compares it pixel by pixel against a committed reference image. The `examples/lvgl_sim_sdl` example in the component repository does exactly this; its pytest script (`pytest_lvgl_sim_sdl.py`) decodes the PNG with the Python standard library — no image library dependency.

The workflow:

1. **Make the UI deterministic.** Nothing in the frame may depend on wall time: no clocks, no animations in flight, no time-based data. The example uses a static screen.
2. **Disable theme transitions.** The LVGL default theme animates widget state changes (a press/release fades the button color over ~150 ms). A frame captured mid-fade differs from run to run. Set in `sdkconfig.defaults`:

   ```text
   CONFIG_LV_THEME_DEFAULT_TRANSITION_TIME=0
   ```

3. **Run headless and deterministic.** Force the framebuffer-only path (see [Getting Started](index.md#headless-runs-no-display)), render once with `lv_refr_now()`, export.
4. **Compare.** The test reads the PNG the application wrote — application and test share the file system, so there is no serial transfer and no intermediate image format.
5. **Update the golden intentionally.** When the UI changed on purpose, inspect the new frame and copy it over the committed golden image.

For interaction tests (press a button, then compare), see the [console command interface](console.md) — it adds scripted input on top of the same export-and-compare loop.
