/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * Raylib platform layer for ESP-IDF, in the shape of upstream's memory port.
 *
 * Despite the .c extension and the location next to the compiled sources, this
 * file is never compiled on its own. CMakeLists.txt puts a configure-time copy
 * of rcore.c in a shadow directory, and at rcore.c's platform hook --
 *
 *     #elif defined(PLATFORM_MEMORY)
 *         #include "platforms/rcore_memory.c"
 *
 * -- a one-line forwarding stub sits at that path and #includes this file by
 * absolute path. The stub's content never changes, so editing this file needs
 * no CMake reconfigure or copy step: the compiler's depfile records it as an
 * input to rcore.c, and Ninja just rebuilds that one object. The quoted
 * include resolves against the copy's own directory first, so this file
 * replaces the submodule's port without touching upstream. Everything a
 * platform port may rely on is already in scope at that point of rcore.c:
 * CoreData and CORE, the config.h limits, rlgl and -- through rlgl.h's
 * software path -- the rlsw declarations and SW_* macros. Frames rendered by
 * rlsw are handed to esp_lcd; see SwapScreenBuffer().
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

/* Framebuffer pixels are RLSW_FRAMEBUFFER_COLOR_TYPE -- the type Kconfig asked
 * for, going out to a panel that expects one of two layouts. Both the panel
 * handout and the byte-swap option are expressed in bytes per pixel so they
 * keep working for whatever type is selected.
 *
 * SW_FRAMEBUFFER_COLOR_SIZE expands to SW_PIXELFORMAT_SIZE[...], an array that
 * only exists where RLSW_IMPLEMENTATION is instantiated. That is this
 * translation unit: rcore.c includes rlgl.h with RLGL_IMPLEMENTATION, which
 * pulls in rlsw.h's implementation. It is one more reason this file has to
 * stay a #include-d part of rcore.c rather than a standalone TU. */
#define FRAME_BPP      SW_FRAMEBUFFER_COLOR_SIZE
#define FRAME_RGB565   (SW_FRAMEBUFFER_COLOR_FORMAT == SW_PIXELFORMAT_COLOR_R5G6B5)

typedef struct {
    uint8_t *pixels;
    uint16_t width;
    uint16_t height;
    uint8_t index;               /* buffer to hand back to free_q once presented */
} present_req_t;

typedef struct {
    /* Present pipeline. At any instant a color buffer is in exactly one of
     * three places: free_q (idle, drawable), the renderer's cur (being drawn),
     * or present_q / the present task (on the wire). Ownership moves only by
     * passing an index through a queue -- there is no semaphore bookkeeping to
     * keep in sync with the swaps. */
    QueueHandle_t free_q;        /* idle buffer indices, depth 2 */
    QueueHandle_t present_q;     /* frames to present, depth 2 */
    SemaphoreHandle_t gone;      /* given once the present task has exited */
    TaskHandle_t present_task;
    uint8_t cur;                 /* buffer index the renderer draws into */
    double last_frame_end;       /* GetTime() of the last SwapScreenBuffer() */
} platform_t;

static platform_t platform;

/* Staging for one band, owned by the present task and reused across bands and
 * frames. flush_cb must consume the pixels before returning (see
 * esp_raylib.h), so reuse is safe. */
static uint8_t *band;
static uint16_t band_rows;
static bool band_alloc_failed;   /* the out-of-memory error is reported once */

/* ------------------------------------------------------------------------- */
/* Present pipeline startup                                                  */
/* ------------------------------------------------------------------------- */

static void present_task(void *arg);

/* Create the ownership queues and the present task. Called from the renderer's
 * first SwapScreenBuffer(). Stack size and priority come from the display
 * config; the priority floor of one above the caller is the pipeline's
 * invariant: the present task must outrun a renderer that blocks only in its
 * pacing wait, or the panel wire starves between frames. */
static bool present_start(void)
{
    /* Both queues hold at most the two color buffers, so present_q can never
     * fill up and free_q remains the one place the renderer ever waits. */
    platform.free_q = xQueueCreate(2, sizeof(uint8_t));
    platform.present_q = xQueueCreate(2, sizeof(present_req_t));
    platform.gone = xSemaphoreCreateBinary();
    if ((platform.free_q == NULL) || (platform.present_q == NULL) ||
            (platform.gone == NULL)) {
        goto fail;
    }

    /* Ownership starts out lopsided, and has to: the first frame is already
     * drawn into rlsw's initial color buffer (index 0), which this very first
     * swap is about to hand to present, so it is not idle. Only the other
     * buffer (index 1) is free for the renderer to draw into after the swap.
     * Seeding that single token is what makes the ownership count exact from
     * frame one -- there is no "both buffers are free" startup fiction. */
    const uint8_t free_idx = 1;
    if (xQueueSend(platform.free_q, &free_idx, 0) != pdTRUE) {
        goto fail;
    }

    const size_t stack = ESP_RAYLIB_PRESENT_STACK_SIZE;

    /* Frame delivery has to outrun a renderer that blocks only in its pacing
     * wait, or the panel wire starves between frames. This is an internal
     * invariant, not a user knob. */
    UBaseType_t caller_prio = uxTaskPriorityGet(NULL);
    UBaseType_t prio = caller_prio + 1;
    if (prio > configMAX_PRIORITIES - 1) {
        /* No higher slot exists when the renderer itself sits at the top
         * priority; say so instead of silently dropping the invariant. */
        ESP_LOGW(TAG, "renderer is at max priority %u; frame pacing cannot be guaranteed",
                 (unsigned)caller_prio);
        prio = configMAX_PRIORITIES - 1;
    }

    BaseType_t ok = xTaskCreate(present_task, "raylib_present", stack, NULL,
                                prio, &platform.present_task);
    if (ok != pdPASS) {
        goto fail;
    }
    return true;

fail:
    if (platform.free_q != NULL) {
        vQueueDelete(platform.free_q);
        platform.free_q = NULL;
    }
    if (platform.present_q != NULL) {
        vQueueDelete(platform.present_q);
        platform.present_q = NULL;
    }
    if (platform.gone != NULL) {
        vSemaphoreDelete(platform.gone);
        platform.gone = NULL;
    }
    return false;
}

/* ------------------------------------------------------------------------- */
/* Frame delivery                                                            */
/* ------------------------------------------------------------------------- */

/*
 * rlsw keeps row 0 at the bottom (the GL convention) while esp_lcd panels
 * address row 0 at the top, so every band walks the source upwards. That
 * avoids a full-frame copy and a separate flip pass; the flip is unconditional
 * because it is what the hardware needs, not a per-panel option.
 *
 * The band is staged, not handed over in place: the rows run backwards, and
 * flush_cb is entitled to a contiguous block. On top of that copy a RGB565
 * framebuffer is optionally byte-swapped.
 */
static void send_band(const uint8_t *src, ptrdiff_t src_stride, uint16_t y, uint16_t rows,
                      uint16_t width, bool swap_bytes)
{
    esp_raylib_display_t *d = esp_raylib_current();
    const size_t row_len = (size_t)width * FRAME_BPP;

    if (band_rows < rows) {
        heap_caps_free(band);
        band = heap_caps_malloc(row_len * rows, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (band == NULL) {
            band_rows = 0;
            /* Every band of every frame would hit this while the heap stays
             * exhausted; one report is enough. */
            if (!band_alloc_failed) {
                band_alloc_failed = true;
                ESP_LOGE(TAG, "no memory for a %u row band", rows);
            }
            return;
        }
        band_alloc_failed = false;
        band_rows = rows;
    }

    uint8_t *dst = band;
    const uint8_t *row = src;

    for (uint16_t i = 0; i < rows; i++) {
        if (FRAME_RGB565 && swap_bytes) {
            const uint16_t *in = (const uint16_t *)row;
            uint16_t *out = (uint16_t *)dst;

            for (uint16_t x = 0; x < width; x++) {
                out[x] = __builtin_bswap16(in[x]);
            }
        } else {
            memcpy(dst, row, row_len);
        }
        dst += row_len;
        row += src_stride;
    }

    /* d points at the object the handle denotes, so it can be handed straight
     * back: the callback gets the display handle, not the raw esp_lcd panel
     * handle -- that lives in user_data (see esp_raylib.h). The callback and
     * its context were copied out of the config at add_display() time. */
    d->flush_cb(d, band, 0, y, width, rows, d->user_data);
}

/*
 * The present task owns the panel hand-off. It stages each band (flip and
 * optional byte swap) and drives flush_cb synchronously, so the renderer
 * never waits for the wire: it hands over a finished color buffer and goes on
 * drawing into an idle one.
 *
 * Once the last band is consumed the buffer is idle again and its index goes
 * back to free_q -- the only way the renderer may ever draw into it again. A
 * renderer that outruns the panel simply finds free_q empty and blocks until
 * a buffer comes back: throttled to the panel rate, which is the best any
 * single panel can show anyway.
 */
static void present_task(void *arg)
{
    (void)arg;

    present_req_t req;
    for (;;) {
        xQueueReceive(platform.present_q, &req, portMAX_DELAY);
        if (req.pixels == NULL) {   /* quit sentinel from present_quiesce() */
            break;
        }

        esp_raylib_display_t *d = esp_raylib_current();
        const size_t row_bytes = (size_t)req.width * FRAME_BPP;
        const uint16_t rows_per_band = (d->chunk_rows > 0) ? d->chunk_rows : req.height;

        for (uint16_t y = 0; y < req.height; y += rows_per_band) {
            const uint16_t rows = (uint16_t)((y + rows_per_band > req.height)
                                             ? (req.height - y) : rows_per_band);

            /* Panel row y comes from renderer row height-1-y; all arithmetic
             * stays signed so the backwards walk cannot wrap. */
            const ptrdiff_t src_row = (ptrdiff_t)req.height - 1 - (ptrdiff_t)y;
            const uint8_t *src = req.pixels + src_row * (ptrdiff_t)row_bytes;
            const ptrdiff_t stride = -(ptrdiff_t)row_bytes;

            send_band(src, stride, y, rows, req.width, d->swap_bytes);
        }

        /* Return the buffer's ownership. Only idle buffers live in free_q,
         * so the renderer can never draw into one that is still on the wire. */
        uint8_t idx = req.index;
        xQueueSend(platform.free_q, &idx, portMAX_DELAY);
    }

    xSemaphoreGive(platform.gone);
    vTaskDelete(NULL);
}

void SwapScreenBuffer(void)
{
    /* The present pipeline starts here, on the renderer's stack context: see
     * present_start() for the present task's stack and priority rules. */
    if (platform.present_task == NULL) {
        if (!present_start()) {
            ESP_LOGE(TAG, "could not start the present task");
            return;
        }
    }

    /* swGetColorBuffer() always returns the default framebuffer's colour
     * attachment (RLSW.framebuffer.color); it does not follow the currently
     * bound framebuffer. A frame left inside BeginTextureMode() therefore
     * presents the default framebuffer's (stale) contents, never the render
     * texture -- so binding 0 here is not what makes the frame valid.
     *
     * It is still needed for the swap below: swSwapColorBuffers() only
     * repoints RLSW.colorBuffer, leaving boundFramebufferId and the depth
     * attachment untouched. swBindFramebuffer(0) resets all three to the
     * default framebuffer, so rlgl stays consistent even if a render texture
     * was still bound when EndDrawing() arrived. */
    swBindFramebuffer(0);

    int fb_width = 0, fb_height = 0;
    uint8_t *pixels = (uint8_t *)swGetColorBuffer(&fb_width, &fb_height);

    /* No size check against a config here: rlglInit() allocated this buffer
     * from CORE.Window.render.*, i.e. exactly what InitWindow() set, so
     * fb_width/height is that size and present_req just carries it along. */
    if ((pixels == NULL) || (fb_width <= 0) || (fb_height <= 0)) {
        ESP_LOGE(TAG, "renderer has no colour buffer");
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

    /* Frame boundary as an ownership hand-off. Give the finished color buffer
     * (cur) to present, swap rlsw's draw target to the other buffer, then
     * claim an idle buffer from free_q before the next frame may draw into it.
     *
     * free_q only ever receives buffers the present task has finished with, so
     * taking a token is both the ownership claim and the throttle: it blocks
     * exactly when the panel is slower than the renderer, and a buffer still
     * on the wire is never handed back to the renderer. */
    const present_req_t req = {
        .pixels = pixels,
        .width = (uint16_t)fb_width,
        .height = (uint16_t)fb_height,
        .index = platform.cur,
    };
    xQueueSend(platform.present_q, &req, portMAX_DELAY);

    swSwapColorBuffers();

    /* swSwapColorBuffers() exchanges color/backColor, so the newly bound draw
     * target is the buffer the present task returns: cur's opposite. Reading
     * the index back from the queue keeps cur derived from ownership rather
     * than toggled independently of it. */
    uint8_t next;
    xQueueReceive(platform.free_q, &next, portMAX_DELAY);
    platform.cur = next;

    /* Frame boundary: block at least one tick so the idle task the watchdog
     * on this core watches gets to run -- a renderer that never blocks trips
     * it within seconds.
     *
     * With SetTargetFPS() this wait is the pacing itself, and it lives here
     * because rcore.c's WaitTime() has no sleep primitive on this toolchain:
     * both its Sleep() and nanosleep() branches are platform-locked, leaving
     * only a busy-wait. EndDrawing() calls it for any frame that comes in
     * under target, so this wait has to reach target -- but without paying an
     * extra tick on every frame. */
    double now = GetTime();
    double period = now - platform.last_frame_end;
    TickType_t ticks = 1;
    if ((CORE.Time.target > 0.0) && (CORE.Time.target > period)) {
        ticks = (TickType_t)((CORE.Time.target - period) * configTICK_RATE_HZ);
        if (ticks < 1) {
            ticks = 1;
        }
    }
    vTaskDelay(ticks);

    /* vTaskDelay(N) wakes on an absolute tick boundary, so N whole ticks can
     * be short of the requested wall time. Top up with blocking delays --
     * never the busy-wait WaitTime() would use -- until the frame measured
     * from the same anchor as EndDrawing() reaches target. This replaces the
     * old unconditional "+1": the spare tick is spent only when the
     * measurement needs it, so the quantisation overshoot is bounded by one
     * tick instead of being added to every frame. */
    while ((CORE.Time.target > 0.0) &&
            (GetTime() - platform.last_frame_end < CORE.Time.target)) {
        vTaskDelay(1);
    }
    platform.last_frame_end = GetTime();
}

/* ------------------------------------------------------------------------- */
/* Window and monitor                                                        */
/* ------------------------------------------------------------------------- */

/* Same shape as the upstream ports: report closed once the window is gone, so
 * a CloseWindow() from anywhere still ends the loop. */
bool WindowShouldClose(void)
{
    return !CORE.Window.ready || CORE.Window.shouldClose;
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
    /* No window system on this target, so there is no native window handle to
     * return -- same as upstream's Memory/Web/DRM ports. The symbol still has
     * to exist: rcore.c lists it among the functions a platform must supply. */
    return NULL;
}

/* There is no window manager here, so no title is ever displayed and nothing
 * reads CORE.Window.title. Pin the field to a string literal (static storage,
 * in .rodata) instead of keeping the caller's pointer -- which can dangle --
 * or a writable RAM copy for a field nobody reads. The signature is fixed by
 * raylib.h, so the argument has to stay even though it is ignored. */
#define WINDOW_TITLE "esp-idf raylib"

void SetWindowTitle(const char *title)
{
    (void)title;
    CORE.Window.title = WINDOW_TITLE;
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
    return CORE.Window.screen.width;
}
int GetMonitorHeight(int monitor)
{
    (void)monitor;
    return CORE.Window.screen.height;
}
int GetMonitorPhysicalWidth(int monitor)
{
    (void)monitor;
    return CORE.Window.screen.width;
}
int GetMonitorPhysicalHeight(int monitor)
{
    (void)monitor;
    return CORE.Window.screen.height;
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
    /* Reset last gamepad button registered state, as the template port does */
    CORE.Input.Gamepad.lastButtonPressed = 0;
}

/* ------------------------------------------------------------------------- */
/* Misc                                                                      */
/* ------------------------------------------------------------------------- */

double GetTime(void)
{
    return (double)(esp_timer_get_time() - esp_raylib_current()->start_us) / 1000000.0;
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
    if (rlGetVersion() != RL_OPENGL_SOFTWARE) {
        ESP_LOGE(TAG, "this component only supports the software renderer");
        return -1;
    }

    /* added is set only by a successful esp_raylib_add_display(), which has
     * already validated cfg (including a non-NULL flush_cb). There is no
     * separate callback-registration step for this check to guard. */
    if (!esp_raylib_current()->added) {
        ESP_LOGE(TAG, "call esp_raylib_add_display() before InitWindow()");
        return -1;
    }

    /* InitWindow() has already written its width/height into
     * CORE.Window.screen.* (rcore.c:642), and rcore.c:669 derives render.*
     * from it right after we return. Do NOT overwrite those: InitWindow() is
     * the single source of truth for display size.
     *
     * The embedded path leaves display.* and currentFbo.* at zero, though,
     * and BeginMode3D() divides by currentFbo.width/height (rcore.c:954) --
     * zero there would produce a NaN aspect. Backfill both from screen.* so
     * every size field agrees with the one value passed to InitWindow(). */
    CORE.Window.display.width = CORE.Window.screen.width;
    CORE.Window.display.height = CORE.Window.screen.height;
    CORE.Window.currentFbo.width = CORE.Window.screen.width;
    CORE.Window.currentFbo.height = CORE.Window.screen.height;

    /* InitWindow() stored its title argument (possibly a stack buffer) in this
     * field before calling us; replace it with the fixed literal. */
    CORE.Window.title = WINDOW_TITLE;
    CORE.Window.ready = true;
    CORE.Storage.basePath = "/";

    esp_raylib_current()->start_us = esp_timer_get_time();

    if (esp_raylib_current()->swap_bytes && !FRAME_RGB565) {
        ESP_LOGW(TAG, "swap_bytes is set but the framebuffer is not RGB565; it will be ignored");
    }

    /* Report the frame format once, here: this is the only place that sees both
     * the compile-time choice and the runtime display, and a panel that shows
     * stripes is almost always a byte-swap or depth that disagrees with what
     * sdkconfig promised. Sizes come from CORE.Window, set by InitWindow(). */
    ESP_LOGI(TAG, "display %ux%u, software renderer, %d bpp framebuffer",
             CORE.Window.screen.width, CORE.Window.screen.height, (int)(FRAME_BPP * 8));
    return 0;
}

/*
 * Stop the present task while the framebuffers are still alive.
 *
 * rcore.c's CloseWindow() calls rlglClose() -- which frees the framebuffers --
 * before ClosePlatform(). Anything still streaming in the present task would
 * therefore be reading freed memory by the time ClosePlatform() runs. The
 * wrapper below runs this first instead, so the in-flight frame finishes
 * against valid pixels and the task is gone before rlglClose() touches them.
 *
 * Drop every frame that is queued but not started: any of them would otherwise
 * be streamed ahead of the quit sentinel. Their indices go back to free_q so
 * the ownership count stays consistent. A frame the task has already dequeued
 * is left to complete -- its pixels are valid at this point.
 */
static void present_quiesce(void)
{
    if (platform.present_task == NULL) {
        return;   /* window closed before the first SwapScreenBuffer() */
    }

    present_req_t stale;
    while (xQueueReceive(platform.present_q, &stale, 0) == pdTRUE) {
        uint8_t idx = stale.index;
        xQueueSend(platform.free_q, &idx, 0);
    }

    const present_req_t quit = { 0 };
    xQueueSend(platform.present_q, &quit, portMAX_DELAY);
    xSemaphoreTake(platform.gone, portMAX_DELAY);
    platform.present_task = NULL;
}

/*
 * rcore.c includes this file at its platform hook (line ~533), above
 * CloseWindow() (~line 726), which holds the only rlglClose() call site.
 * Wrapping it makes present_quiesce() run before rlglClose() frees the
 * framebuffers, which is the ordering ClosePlatform() alone cannot achieve.
 *
 * The nested rlglClose() is not re-expanded -- a macro is never expanded while
 * it is being expanded -- so it calls the real rlgl.h function. This macro is
 * deliberately left defined through the rest of rcore.c: unlike the two
 * file-local macros below, it must not be #undef'd here.
 */
#define rlglClose() (present_quiesce(), rlglClose())

void ClosePlatform(void)
{
    platform.last_frame_end = 0.0;
    platform.cur = 0;

    /* present_quiesce() has already made the task delete itself, so the
     * queues and the band buffer are safe to tear down now. */
    if (platform.free_q != NULL) {
        vQueueDelete(platform.free_q);
        platform.free_q = NULL;
    }
    if (platform.present_q != NULL) {
        vQueueDelete(platform.present_q);
        platform.present_q = NULL;
    }
    if (platform.gone != NULL) {
        vSemaphoreDelete(platform.gone);
        platform.gone = NULL;
    }

    heap_caps_free(band);
    band = NULL;
    band_rows = 0;
    band_alloc_failed = false;
}

/* File-local macros must not leak into the rest of rcore.c. rlglClose() above
 * is the exception: it is the close hook and is intentionally left defined. */
#undef FRAME_BPP
#undef FRAME_RGB565
#undef WINDOW_TITLE
