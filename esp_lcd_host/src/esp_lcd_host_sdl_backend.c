/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SDL backend for the esp_lcd_host panel driver.
 *
 * Every panel framebuffer is uploaded into an SDL streaming texture and
 * presented through the software renderer. The panel framebuffer stays the
 * single source of truth that the GUI renders into and that the screenshot
 * encoder reads; the texture upload copies the shown region once per
 * presented frame.
 *
 * The texture/renderer path is the one output path SDL3 supports on every
 * video driver of the host target: X11 presents through the window surface,
 * while on Wayland (which has no native window surface) SDL emulates it
 * through a texture framebuffer that needs an accelerated renderer behind
 * it. That is why SDL_OPENGLES stays enabled for this component, see the
 * component CMakeLists.txt.
 *
 * The SDL headers come from esp_lcd_host/SDL, the git submodule the component
 * builds with add_subdirectory() in the component CMakeLists.txt.
 */
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "esp_check.h"
#include "esp_log.h"
#include "SDL3/SDL.h"
#include "esp_lcd_host_panel.h"

static const char *TAG = "lcd_host.sdl";

#define HOST_DEFAULT_WINDOW_TITLE "esp_lcd_host"

/* Set when the user closed a preview window (or quit the application from the
 * window manager). Sticky on purpose: an application that polls it once per
 * frame with several panels open still sees the request. */
static bool s_window_close_requested = false;

/* SDL3 initializes subsystems lazily, but doing it explicitly here gives
 * esp_lcd_host_deinit() a well-defined counterpart: the component owns one
 * video subsystem reference for its whole lifetime, no matter how many
 * preview windows are created. SDL_Quit() in deinit() releases it. */
static bool s_sdl_initialized = false;

/* Registry of the SDL backends of the process. esp_lcd_host_pump_events() has
 * no panel handle, so it walks this list to present every dirty window and to
 * drain the shared SDL event queue. All calls into this component are made
 * from a single thread (SDL windows and its event queue are not thread-safe),
 * so the registry needs no locking. */
typedef struct host_sdl_ctx {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    uint8_t *framebuffer;
    size_t framebuffer_size;
    int width;
    int height;
    size_t bytes_per_pixel;
    /* The framebuffer flavor whose byte order differs from the host endianness
     * is byte-swapped into `scratch` on every upload. NULL when no swap is
     * needed (the upload then reads the panel framebuffer directly). */
    bool byteswap;
    uint16_t *scratch;
    bool dirty;
    /* Union of the panel regions drawn since the last present: the pump
     * uploads only this part of the framebuffer into the texture. */
    SDL_Rect dirty_rect;
    /* The screenshot key and what to do when it is pressed, both from the
     * panel configuration. */
    int screenshot_key;
    esp_lcd_host_screenshot_fn_t screenshot_fn;
    void *screenshot_ctx;
    struct host_sdl_ctx *next;
} host_sdl_ctx_t;

static host_sdl_ctx_t *s_backends = NULL;

static void s_backends_add(host_sdl_ctx_t *ctx);

esp_err_t esp_lcd_host_sdl_pixel_format(esp_color_fourcc_t fourcc, uint32_t *ret_format, size_t *ret_bytes)
{
    uint32_t format = 0;
    size_t bytes = 0;

    switch (fourcc) {
    /* Both RGB565 flavors map to SDL_PIXELFORMAT_RGB565, which stores the
     * pixel value in native byte order. The upload byte-swaps the flavor
     * whose memory layout differs from the host, so both display correctly. */
    case ESP_COLOR_FOURCC_RGB16:
    case ESP_COLOR_FOURCC_RGB16_BE:
        format = SDL_PIXELFORMAT_RGB565;
        bytes = 2;
        break;
    case ESP_COLOR_FOURCC_RGB24:
        format = SDL_PIXELFORMAT_RGB24;
        bytes = 3;
        break;
    case ESP_COLOR_FOURCC_BGR24:
        format = SDL_PIXELFORMAT_BGR24;
        bytes = 3;
        break;
    case ESP_COLOR_FOURCC_BGRA32:
        format = SDL_PIXELFORMAT_BGRA32;
        bytes = 4;
        break;
    default:
        ESP_LOGE(TAG, "unsupported color format 0x%" PRIx32, (uint32_t)fourcc);
        return ESP_ERR_NOT_SUPPORTED;
    }

    *ret_format = format;
    *ret_bytes = bytes;
    return ESP_OK;
}

/* One video subsystem reference for the whole component, released by
 * esp_lcd_host_sdl_backend_deinit(). Failing here is the headless case: no
 * video driver can be started (including SDL_VIDEODRIVER pointing at a
 * driver that does not exist), so the caller falls back to a framebuffer-only
 * panel. */
static esp_err_t host_sdl_ensure_init(void)
{
    if (s_sdl_initialized) {
        return ESP_OK;
    }
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        ESP_LOGE(TAG, "failed to initialize SDL video: %s", SDL_GetError());
        return ESP_FAIL;
    }
    s_sdl_initialized = true;
    return ESP_OK;
}

esp_err_t esp_lcd_host_sdl_backend_create(const esp_lcd_host_config_t *config, uint8_t *framebuffer,
                                          size_t framebuffer_size, uint32_t sdl_format, size_t bytes_per_pixel,
                                          void **ret_ctx)
{
    host_sdl_ctx_t *ctx = NULL;
    int scale = config->scale > 0 ? config->scale : 1;

    *ret_ctx = NULL;
    if (!config->create_window) {
        /* Framebuffer only: the content can still be exported as a PNG file,
         * no SDL object needed. */
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(host_sdl_ensure_init(), TAG, "SDL video is unavailable");

    ctx = calloc(1, sizeof(host_sdl_ctx_t));
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_NO_MEM, TAG, "no mem for SDL backend");
    ctx->framebuffer = framebuffer;
    ctx->framebuffer_size = framebuffer_size;
    ctx->bytes_per_pixel = bytes_per_pixel;
    ctx->width = config->width;
    ctx->height = config->height;
    /* RGB565 flavors whose memory layout is the reverse of the host endianness
     * are swapped into a scratch copy during the upload. */
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
    ctx->byteswap = (config->color_format == ESP_COLOR_FOURCC_RGB16_BE);
#else
    ctx->byteswap = (config->color_format == ESP_COLOR_FOURCC_RGB16);
#endif
    if (ctx->byteswap) {
        ctx->scratch = malloc(framebuffer_size);
        if (!ctx->scratch) {
            free(ctx);
            return ESP_ERR_NO_MEM;
        }
    }
    /* The panel installs the function to call once the backend exists. */
    ctx->screenshot_key = config->screenshot_key;

    /* Keep windows resizable so the preview can be zoomed by the user as well.
     * The initial size is the panel size times the requested scale. */
    ctx->window = SDL_CreateWindow(config->window_title ? config->window_title : HOST_DEFAULT_WINDOW_TITLE,
                                   config->width * scale, config->height * scale, SDL_WINDOW_RESIZABLE);
    if (!ctx->window) {
        ESP_LOGE(TAG, "failed to create preview window: %s", SDL_GetError());
        goto fail;
    }
    if (!SDL_SetWindowMinimumSize(ctx->window, config->width, config->height)) {
        ESP_LOGW(TAG, "failed to set window minimum size: %s", SDL_GetError());
    }
    /* Keep the window shape close to the panel shape while the user resizes.
     * SDL scales the panel into the window with the logical presentation set
     * up below, so the pixels stay square. */
    if (!SDL_SetWindowAspectRatio(ctx->window, (float)config->width / (float)config->height,
                                  (float)config->width / (float)config->height)) {
        ESP_LOGW(TAG, "failed to set window aspect ratio: %s", SDL_GetError());
    }

    /* Let SDL pick the renderer instead of naming the software one. An
     * accelerated renderer presents straight to the window: one texture
     * upload and a GPU blit per frame. Naming "software" makes
     * SDL_GetWindowSurface() fall back to SDL's texture-framebuffer emulation
     * on Wayland, which rasterizes in software AND re-uploads the whole window
     * surface on every present -- two extra full-frame copies that also defeat
     * the dirty-rect optimization below. SDL falls back to the software
     * renderer itself if no accelerated driver can be started. The exported
     * PNG never passes through SDL, so the choice cannot affect its bytes. */
    ctx->renderer = SDL_CreateRenderer(ctx->window, NULL);
    if (!ctx->renderer) {
        ESP_LOGE(TAG, "failed to create preview renderer: %s", SDL_GetError());
        goto fail;
    }
    /* Force vsync off even if the application set SDL_RENDER_VSYNC: SDL waits
     * for the frame callback of the compositor, and an occluded preview window
     * never gets one, which would stall esp_lcd_host_pump_events() forever.
     * The application paces the pump loop, so an unsynced present is fine. */
    if (!SDL_SetRenderVSync(ctx->renderer, SDL_RENDERER_VSYNC_DISABLED)) {
        ESP_LOGW(TAG, "failed to disable renderer vsync: %s", SDL_GetError());
    }
    /* The whole panel is the logical scene: SDL scales it to the window, and
     * SDL_RenderPresent() of a dirty frame is all the preview needs. */
    if (!SDL_SetRenderLogicalPresentation(ctx->renderer, config->width, config->height,
                                          SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
        ESP_LOGW(TAG, "failed to set the preview scaling: %s", SDL_GetError());
    }

    ctx->texture = SDL_CreateTexture(ctx->renderer, sdl_format, SDL_TEXTUREACCESS_STREAMING,
                                     config->width, config->height);
    if (!ctx->texture) {
        ESP_LOGE(TAG, "failed to create preview texture: %s", SDL_GetError());
        goto fail;
    }
    /* Sample with nearest neighbor: the preview shows an LCD with a fixed
     * pixel grid, a linear filter would blur it whenever the window size is
     * not an integer multiple of the panel resolution. */
    if (!SDL_SetTextureScaleMode(ctx->texture, SDL_SCALEMODE_NEAREST)) {
        ESP_LOGW(TAG, "failed to set the texture scale mode: %s", SDL_GetError());
    }

    s_backends_add(ctx);
    ESP_LOGI(TAG, "preview window '%s' created (%dx%d, scale %d)",
             config->window_title ? config->window_title : HOST_DEFAULT_WINDOW_TITLE,
             config->width * scale, config->height * scale, scale);
    *ret_ctx = ctx;
    return ESP_OK;

fail:
    if (ctx->texture) {
        SDL_DestroyTexture(ctx->texture);
    }
    if (ctx->renderer) {
        SDL_DestroyRenderer(ctx->renderer);
    }
    if (ctx->window) {
        SDL_DestroyWindow(ctx->window);
    }
    free(ctx->scratch);
    free(ctx);
    return ESP_FAIL;
}

/* Put one backend at the head of the registry. */
static void s_backends_add(host_sdl_ctx_t *ctx)
{
    ctx->next = s_backends;
    s_backends = ctx;
}

esp_err_t esp_lcd_host_sdl_backend_set_screenshot_fn(void *backend,
                                                     esp_lcd_host_screenshot_fn_t fn,
                                                     void *user_ctx)
{
    host_sdl_ctx_t *ctx = backend;
    if (!ctx) {
        return ESP_OK;
    }
    ctx->screenshot_fn = fn;
    ctx->screenshot_ctx = user_ctx;
    return ESP_OK;
}

/* Drop every SDL resource of one backend. */
static void host_sdl_free_objects(host_sdl_ctx_t *ctx)
{
    if (ctx->texture) {
        SDL_DestroyTexture(ctx->texture);
        ctx->texture = NULL;
    }
    if (ctx->renderer) {
        SDL_DestroyRenderer(ctx->renderer);
        ctx->renderer = NULL;
    }
    if (ctx->window) {
        SDL_DestroyWindow(ctx->window);
        ctx->window = NULL;
    }
    free(ctx->scratch);
    ctx->scratch = NULL;
    ctx->dirty = false;
}

/* Close the window of one backend after a close request, keeping the rest of
 * the backend alive: the panel framebuffer still renders and exports PNGs. */
static void host_sdl_close_backend(host_sdl_ctx_t *ctx)
{
    host_sdl_free_objects(ctx);
    /* The window is gone, so it can no longer receive the screenshot key.
     * Leaving the callback installed would let a stray event match this
     * backend (see host_sdl_find_screenshot_owner()). */
    ctx->screenshot_fn = NULL;
    ctx->screenshot_ctx = NULL;
    ESP_LOGI(TAG, "preview window closed by the user");
}

void esp_lcd_host_sdl_backend_delete(void *backend)
{
    host_sdl_ctx_t *ctx = backend;
    if (!ctx) {
        return;
    }
    host_sdl_ctx_t **link = &s_backends;
    while (*link && *link != ctx) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = ctx->next;
    }
    host_sdl_free_objects(ctx);
    free(ctx);
}

/* Clip the region to the panel and union it into the pending dirty rect of
 * the backend. */
static void host_sdl_mark_dirty_rect(host_sdl_ctx_t *ctx, int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    int right = x + width;
    int bottom = y + height;
    x = SDL_max(x, 0);
    y = SDL_max(y, 0);
    right = SDL_min(right, ctx->width);
    bottom = SDL_min(bottom, ctx->height);
    if (x >= right || y >= bottom) {
        return;
    }
    if (ctx->dirty_rect.w == 0 || ctx->dirty_rect.h == 0) {
        ctx->dirty_rect.x = x;
        ctx->dirty_rect.y = y;
        ctx->dirty_rect.w = right - x;
        ctx->dirty_rect.h = bottom - y;
    } else {
        int union_right = SDL_max(ctx->dirty_rect.x + ctx->dirty_rect.w, right);
        int union_bottom = SDL_max(ctx->dirty_rect.y + ctx->dirty_rect.h, bottom);
        ctx->dirty_rect.x = SDL_min(ctx->dirty_rect.x, x);
        ctx->dirty_rect.y = SDL_min(ctx->dirty_rect.y, y);
        ctx->dirty_rect.w = union_right - ctx->dirty_rect.x;
        ctx->dirty_rect.h = union_bottom - ctx->dirty_rect.y;
    }
    ctx->dirty = true;
}

esp_err_t esp_lcd_host_sdl_backend_update(void *backend, const uint8_t *framebuffer, size_t framebuffer_size)
{
    host_sdl_ctx_t *ctx = backend;
    if (!ctx) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(framebuffer == ctx->framebuffer && framebuffer_size == ctx->framebuffer_size,
                        ESP_ERR_INVALID_ARG, TAG, "framebuffer mismatch");
    host_sdl_mark_dirty_rect(ctx, 0, 0, ctx->width, ctx->height);
    return ESP_OK;
}

esp_err_t esp_lcd_host_sdl_backend_update_rect(void *backend, const uint8_t *framebuffer, size_t framebuffer_size,
                                               int x, int y, int width, int height)
{
    host_sdl_ctx_t *ctx = backend;
    if (!ctx) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(framebuffer == ctx->framebuffer && framebuffer_size == ctx->framebuffer_size,
                        ESP_ERR_INVALID_ARG, TAG, "framebuffer mismatch");
    host_sdl_mark_dirty_rect(ctx, x, y, width, height);
    return ESP_OK;
}

/* Compare a pressed key against the configured one. An ASCII letter matches
 * with or without Shift, so that both "s" and "S" reach the panel; every
 * other key has to match exactly. */
static bool host_sdl_key_matches(int configured, SDL_Keycode pressed)
{
    if (configured <= 0) {
        return false;
    }
    if (pressed >= 'A' && pressed <= 'Z') {
        pressed += 'a' - 'A';
    }
    return configured == (int)pressed;
}

/* The screenshot key belongs to the window it was pressed in, so that several
 * panels with different keys do not react to each other. Returns the backend
 * that has to save, or NULL. */
static host_sdl_ctx_t *host_sdl_find_screenshot_owner(SDL_Window *window, SDL_Keycode key)
{
    for (host_sdl_ctx_t *ctx = s_backends; ctx; ctx = ctx->next) {
        /* A closed backend has a NULL window. SDL_GetWindowFromID() also
         * returns NULL for an unknown window id, and comparing two NULLs
         * would make such an event trigger a screenshot on a dead window. */
        if (ctx->window && ctx->window == window && ctx->screenshot_fn
            && host_sdl_key_matches(ctx->screenshot_key, key)) {
            return ctx;
        }
    }
    return NULL;
}

/* Byte-swap the dirty rect of a framebuffer whose byte order differs from the
 * host into the scratch copy, so the native endian upload reads swapped
 * pixels. */
static void host_sdl_byteswap_rect(host_sdl_ctx_t *ctx)
{
    const uint8_t *src = ctx->framebuffer
                         + (size_t)ctx->dirty_rect.y * ctx->width * 2
                         + (size_t)ctx->dirty_rect.x * 2;
    uint16_t *dst = ctx->scratch + (size_t)ctx->dirty_rect.y * ctx->width + ctx->dirty_rect.x;
    for (int y = 0; y < ctx->dirty_rect.h; y++) {
        for (int x = 0; x < ctx->dirty_rect.w; x++) {
            dst[x] = (uint16_t)((src[2 * x] << 8) | src[2 * x + 1]);
        }
        src += (size_t)ctx->width * 2;
        dst += ctx->width;
    }
}

/* Event types this component acts on. SDL_PollEvent() drains the entire
 * process-wide SDL queue; taking only these types leaves pointer, mouse and
 * window events of other SDL consumers in the queue. */
static const Uint32 s_host_sdl_event_types[] = {
    SDL_EVENT_QUIT,
    SDL_EVENT_KEY_DOWN,
    SDL_EVENT_WINDOW_CLOSE_REQUESTED,
};

static void host_sdl_handle_event(const SDL_Event *event)
{
    if (event->type == SDL_EVENT_QUIT) {
        s_window_close_requested = true;
    } else if (event->type == SDL_EVENT_KEY_DOWN) {
        host_sdl_ctx_t *owner = host_sdl_find_screenshot_owner(
            SDL_GetWindowFromID(event->key.windowID), event->key.key);
        if (owner) {
            owner->screenshot_fn(owner->screenshot_ctx);
        }
    } else if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        SDL_Window *closed = SDL_GetWindowFromID(event->window.windowID);
        for (host_sdl_ctx_t *ctx = s_backends; ctx; ctx = ctx->next) {
            /* A closed backend has a NULL window; do not match it against a
             * NULL window id from SDL_GetWindowFromID(). */
            if (ctx->window && ctx->window == closed) {
                /* SDL delivers a close request instead of destroying the
                 * window: an unread one would leave a dead window on screen. */
                host_sdl_close_backend(ctx);
                s_window_close_requested = true;
                break;
            }
        }
    }
    /* The simulator has no input API yet: the screenshot key above is the
     * only input acted on, and every other event type stays in the queue. */
}

static void host_sdl_drain_events(void)
{
    SDL_PumpEvents();
    for (size_t i = 0; i < sizeof(s_host_sdl_event_types) / sizeof(s_host_sdl_event_types[0]); i++) {
        Uint32 type = s_host_sdl_event_types[i];
        SDL_Event event;
        while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, type, type) > 0) {
            host_sdl_handle_event(&event);
        }
    }
}

esp_err_t esp_lcd_host_sdl_backend_pump(void)
{
    /* esp_lcd_host_pump_events() has no panel handle, so it walks the registry;
     * it can also be called before the first panel exists. A framebuffer-only
     * build never initializes SDL, so there is no queue to drain there. */
    if (s_sdl_initialized) {
        host_sdl_drain_events();
    }

    /* Present every dirty window. The renderer is created without vsync (see
     * esp_lcd_host_sdl_backend_create()), so an idle GUI is not throttled
     * here either; the application paces the pump loop. A headless build has
     * no window and nothing to present. */
    for (host_sdl_ctx_t *ctx = s_backends; ctx; ctx = ctx->next) {
        if (!ctx->dirty || !ctx->renderer) {
            continue;
        }
        bool uploaded = true;
        if (ctx->dirty_rect.w > 0 && ctx->dirty_rect.h > 0) {
            /* Upload only the union of the drawn regions: the pitch still
             * describes the whole framebuffer row, the pixel pointer just
             * starts at the first row and column of the dirty rect. */
            const uint8_t *src = ctx->framebuffer
                                 + (size_t)ctx->dirty_rect.y * ctx->width * ctx->bytes_per_pixel
                                 + (size_t)ctx->dirty_rect.x * ctx->bytes_per_pixel;
            if (ctx->byteswap) {
                host_sdl_byteswap_rect(ctx);
                src = (const uint8_t *)ctx->scratch
                      + (size_t)ctx->dirty_rect.y * ctx->width * ctx->bytes_per_pixel
                      + (size_t)ctx->dirty_rect.x * ctx->bytes_per_pixel;
            }
            if (!SDL_UpdateTexture(ctx->texture, &ctx->dirty_rect, src,
                                   (int)((size_t)ctx->width * ctx->bytes_per_pixel))) {
                /* Keep the region dirty so the next pump retries the upload
                 * instead of silently dropping the frame. */
                ESP_LOGE(TAG, "failed to upload the frame: %s", SDL_GetError());
                uploaded = false;
            }
        }
        if (uploaded) {
            ctx->dirty_rect.w = 0;
            ctx->dirty_rect.h = 0;
            ctx->dirty = false;
            if (!SDL_RenderClear(ctx->renderer) ||
                !SDL_RenderTexture(ctx->renderer, ctx->texture, NULL, NULL) ||
                !SDL_RenderPresent(ctx->renderer)) {
                ESP_LOGE(TAG, "failed to present the frame: %s", SDL_GetError());
            }
        }
    }
    return ESP_OK;
}

bool esp_lcd_host_window_close_requested(void)
{
    return s_window_close_requested;
}

esp_err_t esp_lcd_host_sdl_backend_deinit(void)
{
    /* Every backend still holds SDL objects until its panel is deleted.
     * Quitting SDL first would leave those objects pointing at a torn-down
     * subsystem, so refuse instead of silently invalidating live panels. */
    ESP_RETURN_ON_FALSE(s_backends == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "delete all host panels before deinitializing");
    if (s_sdl_initialized) {
        /* SDL_Quit() is the only teardown that reliably releases every
         * subsystem reference, including the ones SDL acquired through its
         * own lazy initialization. */
        SDL_Quit();
        s_sdl_initialized = false;
    }
    s_window_close_requested = false;
    return ESP_OK;
}
