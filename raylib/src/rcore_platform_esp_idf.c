/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * Raylib platform layer for ESP-IDF, in the shape of upstream's memory port.
 *
 * Despite the .c extension and the location next to the compiled sources, this
 * file is never compiled on its own: CMakeLists.txt copies it next to a
 * configure-time copy of rcore.c (as platforms/rcore_memory.c in the shadow
 * directory), and the copy includes it at rcore.c's platform hook:
 *
 *     #elif defined(PLATFORM_MEMORY)
 *         #include "platforms/rcore_memory.c"
 *
 * The quoted include resolves against the copy's own directory first, so this
 * file replaces the submodule's port without touching upstream. Everything a
 * platform port may rely on is already in scope at that point of rcore.c:
 * CoreData and CORE, the config.h limits, rlgl and -- through rlgl.h's
 * software path -- the rlsw declarations and SW_* macros. Frames rendered by
 * rlsw are handed to esp_lcd; see SwapScreenBuffer().
 *
 * The submodule's own rcore_memory.c cannot be reused instead: its functions
 * land in this same translation unit, so they can be neither overridden
 * (redefinition) nor macro-renamed (rcore.c's call sites would be renamed
 * with them), and its InitPlatform() allocates a full RGBA8888 frame that
 * its SwapScreenBuffer() copies into -- a second framebuffer this target has
 * neither the memory nor the use for.
 *
 * Warnings carried by the surrounding vendored text are silenced per
 * translation unit in CMakeLists.txt; nothing here needs to know about them.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_raylib.h"
#include "esp_raylib_private.h"

/* rlsw uses 0 for the default framebuffer, the value glBindFramebuffer(3, 0)
 * hands over. */
#define SW_DEFAULT_FRAMEBUFFER 0

/* Framebuffer pixels are RLSW_FRAMEBUFFER_COLOR_TYPE -- the type Kconfig asked
 * for, going out to a panel that expects one of two layouts. Both the panel
 * handout and the byte-swap option are expressed in bytes per pixel so they
 * keep working for whatever type is selected. */
#define FRAME_BPP      SW_FRAMEBUFFER_COLOR_SIZE
#define FRAME_RGB565   (SW_FRAMEBUFFER_COLOR_FORMAT == SW_PIXELFORMAT_COLOR_R5G6B5)

typedef struct {
    uint8_t *pixels;
    uint16_t width;
    uint16_t height;
    uint8_t index;
} present_req_t;

typedef struct {
    /* Present pipeline: the renderer hands a finished double-buffer color
     * buffer to the present task and keeps rendering into the other one. */
    QueueHandle_t present_q;
    SemaphoreHandle_t done[2];   /* [i] given once buffer i is off the wire */
    TaskHandle_t present_task;
    uint8_t cur;                 /* color buffer the renderer draws into */
    double last_frame_end;       /* GetTime() of the last SwapScreenBuffer() */
} platform_t;

static platform_t platform;

/* Staging for one band, owned by the present task and reused across bands and
 * frames. flush_cb must consume the pixels before returning (see
 * esp_raylib.h), so reuse is safe. */
static uint8_t *band;
static uint16_t band_rows;

/* ------------------------------------------------------------------------- */
/* Present pipeline startup                                                  */
/* ------------------------------------------------------------------------- */

static void present_task(void *arg);

/* Create the queue, the done semaphores and the present task. Called from the
 * renderer's first SwapScreenBuffer(), so the task can be given one priority
 * above the caller: it must never starve behind a renderer that never blocks. */
static bool present_start(void)
{
    platform.present_q = xQueueCreate(1, sizeof(present_req_t));
    platform.done[0] = xSemaphoreCreateBinary();
    platform.done[1] = xSemaphoreCreateBinary();
    if ((platform.present_q == NULL) || (platform.done[0] == NULL) ||
        (platform.done[1] == NULL)) {
        goto fail;
    }
    /* both buffers start off the wire */
    if ((xSemaphoreGive(platform.done[0]) != pdTRUE) ||
        (xSemaphoreGive(platform.done[1]) != pdTRUE)) {
        goto fail;
    }

    BaseType_t ok = xTaskCreate(present_task, "raylib_present", 4096, NULL,
                                uxTaskPriorityGet(NULL) + 1, &platform.present_task);
    return (ok == pdPASS) && (platform.present_task != NULL);

fail:
    if (platform.present_q != NULL) {
        vQueueDelete(platform.present_q);
        platform.present_q = NULL;
    }
    if (platform.done[0] != NULL) {
        vSemaphoreDelete(platform.done[0]);
        platform.done[0] = NULL;
    }
    if (platform.done[1] != NULL) {
        vSemaphoreDelete(platform.done[1]);
        platform.done[1] = NULL;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* Frame delivery                                                            */
/* ------------------------------------------------------------------------- */

/*
 * The renderer keeps row 0 at the bottom (the GL convention) while panels
 * expect row 0 at the top. Walking the source upwards, row by row, avoids a
 * full-frame copy and a separate flip pass.
 *
 * The band is staged, not handed over in place: the rows may run backwards,
 * and flush_cb is entitled to a contiguous block either way. On top of that
 * copy a RGB565 framebuffer is optionally byte-swapped.
 */
static void send_band(const uint8_t *src, ptrdiff_t src_stride, uint16_t y, uint16_t rows,
                      uint16_t width, bool swap_bytes)
{
    const esp_raylib_display_config_t *cfg = &esp_raylib_state.cfg;
    const size_t row_len = (size_t)width * FRAME_BPP;

    if (band_rows < rows) {
        heap_caps_free(band);
        band = heap_caps_malloc(row_len * rows, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (band == NULL) {
            band_rows = 0;
            ESP_LOGE(TAG, "no memory for a %u row band", rows);
            return;
        }
        band_rows = rows;
    }

    uint8_t *dst = band;
    const uint8_t *row = src;

    for (uint16_t i = 0; i < rows; i++) {
        if (FRAME_RGB565 && swap_bytes) {
            const uint16_t *in = (const uint16_t *)row;
            uint16_t *out = (uint16_t *)dst;

            for (uint16_t x = 0; x < width; x++) {
                out[x] = (uint16_t)((in[x] << 8) | (in[x] >> 8));
            }
        } else {
            memcpy(dst, row, row_len);
        }
        dst += row_len;
        row += src_stride;
    }

    cfg->flush_cb(cfg->handle, (const uint16_t *)band, 0, y, width, rows);
}

/*
 * The present task owns the panel hand-off. It stages each band (flip and
 * optional byte swap) and drives flush_cb synchronously, so the renderer
 * never waits for the wire: it queues a finished color buffer and goes on
 * drawing into the other one.
 *
 * Before a swap makes a buffer the draw target again, the renderer waits on
 * that buffer's done semaphore, which the present task gives after its last
 * band. As long as rendering takes longer than the panel transfer the wait
 * never bites; if it does not, the renderer is throttled to the panel rate,
 * which is the best any single panel can show anyway.
 */
static void present_task(void *arg)
{
    (void)arg;

    present_req_t req;
    for (;;) {
        xQueueReceive(platform.present_q, &req, portMAX_DELAY);

        const esp_raylib_display_config_t *cfg = &esp_raylib_state.cfg;
        const size_t row_bytes = (size_t)req.width * FRAME_BPP;
        const uint16_t band = (cfg->chunk_rows > 0) ? cfg->chunk_rows : req.height;

        for (uint16_t y = 0; y < req.height; y += band) {
            const uint16_t rows = (uint16_t)((y + band > req.height) ? (req.height - y) : band);

            /* Panel row y comes from renderer row height-1-y when inverted;
             * all arithmetic stays signed so the backwards walk cannot wrap. */
            const ptrdiff_t src_row = cfg->invert ? (ptrdiff_t)req.height - 1 - (ptrdiff_t)y
                                      : (ptrdiff_t)y;
            const uint8_t *src = req.pixels + src_row * (ptrdiff_t)row_bytes;
            const ptrdiff_t stride = cfg->invert ? -(ptrdiff_t)row_bytes : (ptrdiff_t)row_bytes;

            send_band(src, stride, y, rows, req.width, cfg->swap_bytes);
        }

        xSemaphoreGive(platform.done[req.index]);
    }
}

void SwapScreenBuffer(void)
{
    const esp_raylib_display_config_t *cfg = &esp_raylib_state.cfg;

    if (!esp_raylib_state.configured) {
        ESP_LOGE(TAG, "SwapScreenBuffer() before esp_raylib_set_display()");
        return;
    }

    /* The present pipeline starts here, on the renderer's stack context: see
     * present_start() for why the priority is the caller's plus one. */
    if (platform.present_task == NULL) {
        if (!present_start()) {
            ESP_LOGE(TAG, "could not start the present task");
            return;
        }
    }

    /* A frame is only taken while the default framebuffer is bound: the color
     * buffer swGetColorBuffer() reports follows whatever framebuffer is
     * current, and after a single BeginTextureMode() that is the user's
     * render texture. */
    swBindFramebuffer(SW_DEFAULT_FRAMEBUFFER);

    int fb_width = 0, fb_height = 0;
    uint8_t *pixels = (uint8_t *)swGetColorBuffer(&fb_width, &fb_height);

    if ((pixels == NULL) || (fb_width <= 0) || (fb_height <= 0)) {
        ESP_LOGE(TAG, "renderer has no colour buffer");
        return;
    }

    if ((fb_width != cfg->width) || (fb_height != cfg->height)) {
        ESP_LOGE(TAG, "renderer is %dx%d but the display is %ux%u",
                 fb_width, fb_height, cfg->width, cfg->height);
        return;
    }

    /* One report at startup: a striped panel is almost always a byte-swap or
     * format that disagrees with what sdkconfig promised. */
    static bool reported;
    if (!reported) {
        reported = true;
        ESP_LOGI(TAG, "framebuffer is in %s",
                 esp_ptr_external_ram(pixels) ? "PSRAM" : "internal RAM");
    }

    /* After the swap the other buffer becomes the draw target; it must be off
     * the wire first. */
    xSemaphoreTake(platform.done[platform.cur ^ 1], portMAX_DELAY);

    const present_req_t req = {
        .pixels = pixels,
        .width = (uint16_t)fb_width,
        .height = (uint16_t)fb_height,
        .index = platform.cur,
    };
    xQueueSend(platform.present_q, &req, portMAX_DELAY);

    swSwapColorBuffers();
    platform.cur ^= 1;

    /* Frame boundary: the one point the renderer is guaranteed to reach
     * between two frames of pure compute. Block for at least one tick so the
     * idle task the watchdog on this core watches gets to run; a renderer
     * that never blocks trips the task watchdog within seconds.
     *
     * With SetTargetFPS() this wait is the pacing itself, mirroring the
     * accounting rcore.c's EndDrawing() does right after this call: wait out
     * target minus the period measured from the previous swap, from real
     * timestamps, so a long frame re-anchors instead of stacking catch-up
     * waits. Waiting here matters because rcore.c's WaitTime() has no sleep
     * primitive on this toolchain -- its Sleep() branch is guarded by _WIN32
     * and its nanosleep() branch by __linux__ -- and falls through to the
     * SUPPORT_PARTIALBUSY_WAIT_LOOP spin, which keeps the CPU pegged just the
     * same. Ending the wait past the deadline leaves the frame time
     * EndDrawing() measures at or over the target, so it never runs. */
    double now = GetTime();
    double period = now - platform.last_frame_end;
    TickType_t ticks = 1;
    if ((CORE.Time.target > 0.0) && (CORE.Time.target > period)) {
        ticks = (TickType_t)((CORE.Time.target - period) * configTICK_RATE_HZ) + 1;
    }
    vTaskDelay(ticks);
    platform.last_frame_end = GetTime();
}

/* ------------------------------------------------------------------------- */
/* Window and monitor                                                        */
/* ------------------------------------------------------------------------- */

/* Same shape as the upstream ports: report closed once the window is gone, so
 * a CloseWindow() from anywhere still ends the loop. */
bool WindowShouldClose(void)
{
    if (CORE.Window.ready) {
        return CORE.Window.shouldClose;
    }
    return true;
}

void ToggleFullscreen(void) { }
void ToggleBorderlessWindowed(void) { }
void MaximizeWindow(void) { }
void MinimizeWindow(void) { }
void RestoreWindow(void) { }
void SetWindowState(unsigned int flags)
{
    (void)flags;
}
void ClearWindowState(unsigned int flags)
{
    (void)flags;
}
void SetWindowIcon(Image image)
{
    (void)image;
}
void SetWindowIcons(Image *images, int count)
{
    (void)images;
    (void)count;
}
void SetWindowPosition(int x, int y)
{
    (void)x;
    (void)y;
}
void SetWindowMonitor(int monitor)
{
    (void)monitor;
}
void SetWindowMinSize(int width, int height)
{
    (void)width;
    (void)height;
}
void SetWindowMaxSize(int width, int height)
{
    (void)width;
    (void)height;
}
void SetWindowSize(int width, int height)
{
    (void)width;
    (void)height;
}
void SetWindowOpacity(float opacity)
{
    (void)opacity;
}
void SetWindowFocused(void) { }

void *GetWindowHandle(void)
{
    return esp_raylib_state.cfg.handle;
}

void SetWindowTitle(const char *title)
{
    CORE.Window.title = title;
}

Vector2 GetWindowPosition(void)
{
    Vector2 pos = { 0.0f, 0.0f };
    return pos;
}

Vector2 GetWindowScaleDPI(void)
{
    Vector2 scale = { 1.0f, 1.0f };
    return scale;
}

int GetMonitorCount(void)
{
    return 1;
}
int GetCurrentMonitor(void)
{
    return 0;
}

Vector2 GetMonitorPosition(int monitor)
{
    (void)monitor;
    Vector2 pos = { 0.0f, 0.0f };
    return pos;
}

int GetMonitorWidth(int monitor)
{
    (void)monitor;
    return esp_raylib_state.cfg.width;
}
int GetMonitorHeight(int monitor)
{
    (void)monitor;
    return esp_raylib_state.cfg.height;
}
int GetMonitorPhysicalWidth(int monitor)
{
    (void)monitor;
    return esp_raylib_state.cfg.width;
}
int GetMonitorPhysicalHeight(int monitor)
{
    (void)monitor;
    return esp_raylib_state.cfg.height;
}
int GetMonitorRefreshRate(int monitor)
{
    (void)monitor;
    return 60;
}

const char *GetMonitorName(int monitor)
{
    (void)monitor;
    return "esp_lcd";
}

/* ------------------------------------------------------------------------- */
/* Clipboard, cursor and input have nothing to attach to on a bare panel      */
/* ------------------------------------------------------------------------- */

void SetClipboardText(const char *text)
{
    (void)text;
}
const char *GetClipboardText(void)
{
    return "";
}

Image GetClipboardImage(void)
{
    Image img = { 0 };
    return img;
}

void ShowCursor(void) { }
void HideCursor(void) { }
void EnableCursor(void) { }
void DisableCursor(void) { }

void SetMousePosition(int x, int y)
{
    (void)x;
    (void)y;
}
void SetMouseCursor(int cursor)
{
    (void)cursor;
}
int SetGamepadMappings(const char *mappings)
{
    (void)mappings;
    return 0;
}

void SetGamepadVibration(int gamepad, float leftMotor, float rightMotor, float duration)
{
    (void)gamepad;
    (void)leftMotor;
    (void)rightMotor;
    (void)duration;
}

const char *GetKeyName(int key)
{
    (void)key;
    return "";
}

void PollInputEvents(void)
{
    for (int i = 0; i < MAX_KEYBOARD_KEYS; i++) {
        CORE.Input.Keyboard.previousKeyState[i] = CORE.Input.Keyboard.currentKeyState[i];
        CORE.Input.Keyboard.keyRepeatInFrame[i] = 0;
    }
    CORE.Input.Keyboard.keyPressedQueueCount = 0;
    CORE.Input.Keyboard.charPressedQueueCount = 0;
}

/* ------------------------------------------------------------------------- */
/* Misc                                                                      */
/* ------------------------------------------------------------------------- */

double GetTime(void)
{
    return (double)(esp_timer_get_time() - esp_raylib_state.start_us) / 1000000.0;
}

void OpenURL(const char *url)
{
    ESP_LOGW(TAG, "OpenURL() is not supported on this target: %s", (url != NULL) ? url : "");
}

/* ------------------------------------------------------------------------- */
/* Platform lifecycle                                                        */
/* ------------------------------------------------------------------------- */

int InitPlatform(void)
{
    if (!esp_raylib_state.configured) {
        ESP_LOGE(TAG, "call esp_raylib_set_display() before InitWindow()");
        return -1;
    }

    if (rlGetVersion() != RL_OPENGL_SOFTWARE) {
        ESP_LOGE(TAG, "this component only supports the software renderer");
        return -1;
    }

    CORE.Window.display.width = esp_raylib_state.cfg.width;
    CORE.Window.display.height = esp_raylib_state.cfg.height;
    CORE.Window.screen.width = esp_raylib_state.cfg.width;
    CORE.Window.screen.height = esp_raylib_state.cfg.height;
    CORE.Window.render.width = esp_raylib_state.cfg.width;
    CORE.Window.render.height = esp_raylib_state.cfg.height;
    CORE.Window.currentFbo.width = esp_raylib_state.cfg.width;
    CORE.Window.currentFbo.height = esp_raylib_state.cfg.height;

    CORE.Window.ready = true;
    CORE.Storage.basePath = "/";

    esp_raylib_state.start_us = esp_timer_get_time();

    /* Report the frame format once, here: this is the only place that sees both
     * the compile-time choice and the runtime display, and a panel that shows
     * stripes is almost always a byte-swap or depth that disagrees with what
     * sdkconfig promised. */
    ESP_LOGI(TAG, "display %ux%u, software renderer, %d bpp framebuffer",
             esp_raylib_state.cfg.width, esp_raylib_state.cfg.height, (int)(FRAME_BPP * 8));
    return 0;
}

void ClosePlatform(void)
{
    esp_raylib_state.configured = false;
    platform.last_frame_end = 0.0;
    if (platform.present_task != NULL) {
        vTaskDelete(platform.present_task);
        platform.present_task = NULL;
    }
    heap_caps_free(band);
    band = NULL;
    band_rows = 0;
}

/* File-local macros must not leak into the rest of rcore.c. */
#undef SW_DEFAULT_FRAMEBUFFER
#undef FRAME_BPP
#undef FRAME_RGB565
