/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SDL backend for the esp_lcd_sim panel driver.
 *
 * The panel framebuffer is the single source of truth: the GUI renders into it,
 * the screenshot encoder reads it, and each presented frame uploads the dirty
 * region into an SDL streaming texture. SDL picks the renderer; on Wayland,
 * which has no native window surface, an accelerated (OpenGL ES) renderer is
 * required to present at all -- hence SDL_OPENGLES stays enabled (see the
 * component CMakeLists.txt).
 *
 * With show_board_frame, a static development-board texture is drawn once and
 * the LCD is composited into its screen cut-out. The board never enters the
 * framebuffer, so the exported PNG stays LCD-only. If the board cannot be
 * built, the LCD shows on a plain background instead.
 */
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <inttypes.h>
#include "esp_check.h"
#include "esp_log.h"
#include "SDL3/SDL.h"
#include "esp_lcd_sim_panel.h"

static const char *TAG = "lcd_sim.sdl";

#define SIM_DEFAULT_WINDOW_TITLE "esp_lcd_sim"

/* Maximum number of pending dirty regions per preview. Partial flushes keep
 * their own regions instead of one union that grows towards the full frame;
 * when the list is full, new regions merge into the one that grows least. */
#define SIM_DIRTY_RECTS_MAX 8

/* The development-board look is fixed at compile time: the preview shows one
 * stock display module, not a board kit the application can re-skin. Only the
 * parts that mean something in the simulation are drawn -- the PCB, the
 * display bezel with the screen cut-out, a silkscreen name, the power LED and
 * the mounting holes. Connectors, headers, buttons and an MCU package are
 * deliberately absent: nothing in the simulation reacts to them, so drawing
 * them would only suggest controls that do not exist.
 *
 * Geometry is in logical (panel) pixels. The screen cut-out keeps the exact
 * panel resolution; the margins are the board area around it. Top and bottom
 * margins match, so the screen sits centered in the board instead of riding
 * high above a crowded bottom strip. */
#define SIM_BOARD_BACKGROUND     0x0e1116u  /* Backdrop, also the preview letterbox color */
#define SIM_BOARD_PCB            0x123b2au  /* PCB body */
#define SIM_BOARD_PCB_EDGE       0x0a2619u  /* PCB outline */
#define SIM_BOARD_BEZEL          0x1b1f24u  /* Display module bezel */
#define SIM_BOARD_BEZEL_HI       0x2c3138u  /* Bezel top highlight */
#define SIM_BOARD_SCREEN         0x05070au  /* Screen cut-out behind the LCD */
#define SIM_BOARD_SILK           0xc9d4cfu  /* Silkscreen text and markings */
#define SIM_BOARD_GOLD           0xd9b25au  /* Gold accents above and below the screen */
#define SIM_BOARD_METAL          0x8a929cu  /* Mounting hole rings */
#define SIM_BOARD_DARK           0x1a1d22u  /* Dark plastic (LED base) */
#define SIM_BOARD_LED            0x54e08au  /* Power LED */

#define SIM_BOARD_MARGIN_X       32         /* Board area left and right of the screen */
#define SIM_BOARD_MARGIN_TOP     56         /* Board area above the screen (name, power LED) */
#define SIM_BOARD_MARGIN_BOTTOM  56         /* Board area below the screen (component mark) */
#define SIM_BOARD_CORNER_RADIUS  14         /* PCB corner radius */
#define SIM_BOARD_SHADOW_OFFSET  5          /* Drop shadow offset down and right */

#define SIM_BOARD_NAME           "LCD Simulator"
#define SIM_BOARD_MARK           "Espressif Systems"

/* Set when the user closed a preview window (or quit the application from the
 * window manager). Sticky on purpose: an application that polls it once per
 * frame with several panels open still sees the request. */
static bool s_window_close_requested = false;

/* SDL3 initializes subsystems lazily, but doing it explicitly here gives
 * esp_lcd_sim_deinit() a well-defined counterpart: the component owns one
 * video subsystem reference for its whole lifetime, no matter how many
 * preview windows are created. SDL_Quit() in deinit() releases it. */
static bool s_sdl_initialized = false;

/* Registry of the SDL backends of the process. esp_lcd_sim_pump_events() has
 * no panel handle, so it walks this list to present every dirty window and to
 * drain the shared SDL event queue. All calls into this component are made
 * from a single thread (SDL windows and its event queue are not thread-safe),
 * so the registry needs no locking. */
typedef struct sim_sdl_ctx {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    /* Static, pre-rendered board texture. NULL when show_board_frame is off,
     * or when building it failed (the LCD then sits on a plain background). */
    SDL_Texture *board_texture;
    SDL_FRect screen_rect;          /* where the LCD is composited (board screen or full scene) */
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
    /* Panel regions drawn since the last present: the pump uploads each one
     * into the texture, so scattered partial flushes do not add up to a
     * full-frame upload. Touching regions merge into one. */
    SDL_Rect dirty_rects[SIM_DIRTY_RECTS_MAX];
    int dirty_count;
    /* The window size in pixels the last present was made for. The pump
     * re-presents whenever the window no longer matches, see the pump loop. */
    int presented_pixel_w;
    int presented_pixel_h;
    /* Last pointer position that mapped into the panel, kept for the hover
     * reports of esp_lcd_sim_mouse_get() after the pointer moves off the
     * screen area again. */
    int mouse_x;
    int mouse_y;
    /* The screenshot key and what to do when it is pressed, both from the
     * panel configuration. */
    int screenshot_key;
    esp_lcd_sim_backend_cb_t screenshot_fn;
    void *screenshot_ctx;
    struct sim_sdl_ctx *next;
} sim_sdl_ctx_t;

static sim_sdl_ctx_t *s_backends = NULL;

/* Put one backend at the head of the registry. */
static void s_backends_add(sim_sdl_ctx_t *ctx)
{
    ctx->next = s_backends;
    s_backends = ctx;
}

esp_err_t esp_lcd_sim_sdl_pixel_format(esp_color_fourcc_t fourcc, uint32_t *ret_format, size_t *ret_bytes)
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

/* Map a packed 0xRRGGBB board color onto the surface pixel format. */
static Uint32 sim_board_color(SDL_Surface *surface, Uint32 rgb)
{
    return SDL_MapSurfaceRGB(surface, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb);
}

/* Same, with an alpha channel, for the drop shadow and the LED glow. */
static Uint32 sim_board_rgba(SDL_Surface *surface, Uint32 rgb, Uint8 alpha)
{
    return SDL_MapSurfaceRGBA(surface, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb, alpha);
}

/* Fill a solid rectangle of the board surface. `color` is the mapped pixel
 * value of the surface format. */
static bool sim_board_fill(SDL_Surface *surface, int x, int y, int w, int h, Uint32 color)
{
    if (w <= 0 || h <= 0) {
        return true;
    }
    SDL_Rect rect = {x, y, w, h};
    return SDL_FillSurfaceRect(surface, &rect, color);
}

/* Fill a rectangle with circular corners, one horizontal span per row. The
 * corner inset comes from the circle equation, so the result is a real round
 * corner rather than the stepped cut-outs of a cheap approximation. */
static bool sim_board_rounded_rect(SDL_Surface *surface, int x, int y, int w, int h, int r, Uint32 color)
{
    if (w <= 0 || h <= 0) {
        return true;
    }
    if (r <= 0) {
        return sim_board_fill(surface, x, y, w, h, color);
    }
    r = SDL_min(r, SDL_min(w, h) / 2);
    for (int dy = 0; dy < h; dy++) {
        int dist = 0;
        if (dy < r) {
            dist = r - dy;
        } else if (dy >= h - r) {
            dist = dy - (h - r) + 1;
        }
        int inset = 0;
        if (dist > 0) {
            double d = dist - 0.5;
            inset = r - (int)SDL_sqrt((double)r * r - d * d);
        }
        sim_board_fill(surface, x + inset, y + dy, w - 2 * inset, 1, color);
    }
    return true;
}

/* A soft drop shadow behind the PCB slab: a faint fringe plus a darker core,
 * both offset down and right. Translucent, so it blends over the backdrop the
 * preview composites the board onto. */
static void sim_board_shadow(SDL_Surface *surface, int x, int y, int w, int h, int r, int offset)
{
    if (offset <= 0) {
        return;
    }
    Uint32 fringe = sim_board_rgba(surface, 0x000000u, 40);
    Uint32 core = sim_board_rgba(surface, 0x000000u, 90);
    sim_board_rounded_rect(surface, x + offset - 2, y + offset - 2, w + 4, h + 4, r + 2, fringe);
    sim_board_rounded_rect(surface, x + offset, y + offset, w, h, r, core);
}

/* The silkscreen is drawn with a tiny built-in font so the board stays
 * asset-free. One glyph is seven rows of '.' and '#' read top to bottom, an
 * empty string being a row the letter does not reach: x-height letters start
 * at row 2, ascenders at row 0 and the baseline is row 6. Narrow letters carry
 * their own width, so a line keeps the rhythm of real silkscreen instead of a
 * monospace grid. The table covers a-z, 0-9 and a few marks; uppercase
 * letters fold to their lowercase shapes, anything else advances like a
 * space. */
typedef struct {
    char letter;
    uint8_t width;
    const char *rows[7];
} sim_silk_glyph_t;

static const sim_silk_glyph_t sim_silk_font[] = {
    {'a', 5, {"", "", ".###.", "....#", ".####", "#...#", ".####"}},
    {'b', 5, {"#....", "#....", "####.", "#...#", "#...#", "#...#", "####."}},
    {'c', 5, {"", "", ".###.", "#...#", "#....", "#....", ".###."}},
    {'d', 5, {"....#", "....#", ".###.", "#...#", "#...#", "#...#", ".###."}},
    {'e', 5, {"", "", ".###.", "#...#", "#####", "#....", ".###."}},
    {'f', 4, {"..##", ".#..", "####", ".#..", ".#..", ".#..", ".#.."}},
    {'g', 5, {"", "", ".###.", "#...#", "#...#", ".####", "....#"}},
    {'h', 5, {"#....", "#....", "####.", "#...#", "#...#", "#...#", "#...#"}},
    {'i', 3, {".#.", "", ".#.", ".#.", ".#.", ".#.", ".#."}},
    {'j', 3, {"..#", "", "..#", "..#", "..#", "..#", ".##"}},
    {'k', 5, {"#....", "#....", "#..#.", "#.#..", "##...", "#.#..", "#..#."}},
    {'l', 3, {".#.", ".#.", ".#.", ".#.", ".#.", ".#.", ".#."}},
    {'m', 5, {"", "", "#####", "#.#.#", "#.#.#", "#.#.#", "#.#.#"}},
    {'n', 5, {"", "", "####.", "#...#", "#...#", "#...#", "#...#"}},
    {'o', 5, {"", "", ".###.", "#...#", "#...#", "#...#", ".###."}},
    {'p', 5, {"", "", "####.", "#...#", "####.", "#....", "#...."}},
    {'q', 5, {"", "", ".###.", "#...#", "#...#", ".####", "....#"}},
    {'r', 5, {"", "", "####.", "#..#.", "#....", "#....", "#...."}},
    {'s', 5, {"", "", ".####", "#....", ".###.", "....#", "####."}},
    {'t', 4, {".#..", ".#..", "####", ".#..", ".#..", ".#..", ".#.."}},
    {'u', 5, {"", "", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'v', 5, {"", "", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
    {'w', 5, {"", "", "#...#", "#...#", "#.#.#", "#.#.#", ".#.#."}},
    {'x', 5, {"", "", "#...#", ".#.#.", "..#..", ".#.#.", "#...#"}},
    {'y', 5, {"", "", "#...#", "#...#", ".#.#.", "..#..", ".#..."}},
    {'z', 5, {"", "", "#####", "...#.", "..#..", ".#...", "#####"}},
    {'0', 5, {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}},
    {'1', 5, {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'2', 5, {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}},
    {'3', 5, {".###.", "#...#", "....#", "..##.", "....#", "#...#", ".###."}},
    {'4', 5, {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}},
    {'5', 5, {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}},
    {'6', 5, {".###.", "#....", "#....", "####.", "#...#", "#...#", ".###."}},
    {'7', 5, {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."}},
    {'8', 5, {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}},
    {'9', 5, {".###.", "#...#", "#...#", ".####", "....#", "....#", ".###."}},
    {'-', 5, {"", "", "", "#####", "", "", ""}},
    {'_', 5, {"", "", "", "", "", "", "#####"}},
    {'.', 1, {"", "", "", "", "", "", "#"}},
    {' ', 3, {"", "", "", "", "", "", ""}},
};

static const sim_silk_glyph_t *sim_silk_glyph(char letter)
{
    /* The font only carries lowercase shapes: fold uppercase so a mark like
     * "Espressif Systems" renders instead of leaving a blank per capital. */
    if (letter >= 'A' && letter <= 'Z') {
        letter += 'a' - 'A';
    }
    for (size_t i = 0; i < sizeof(sim_silk_font) / sizeof(sim_silk_font[0]); i++) {
        if (sim_silk_font[i].letter == letter) {
            return &sim_silk_font[i];
        }
    }
    return NULL;
}

/* Width of a line in font pixels (glyph widths plus one pixel of spacing). */
static int sim_silk_units(const char *text)
{
    int units = 0;
    for (const char *p = text; *p; p++) {
        const sim_silk_glyph_t *glyph = sim_silk_glyph(*p);
        units += (glyph ? glyph->width : 3) + 1;
    }
    return units > 0 ? units - 1 : 0;
}

/* Draw a line with its left edge at x, every font pixel a scale-pixel square. */
static void sim_silk_draw(SDL_Surface *surface, int x, int y, const char *text, Uint32 color, int scale)
{
    for (const char *p = text; *p; p++) {
        const sim_silk_glyph_t *glyph = sim_silk_glyph(*p);
        if (glyph) {
            for (int row = 0; row < 7; row++) {
                for (int col = 0; glyph->rows[row][col] != '\0'; col++) {
                    if (glyph->rows[row][col] == '#') {
                        sim_board_fill(surface, x + col * scale, y + row * scale, scale, scale, color);
                    }
                }
            }
        }
        x += ((glyph ? glyph->width : 3) + 1) * scale;
    }
}

/* Write the board name centered on the board width. The scale is the largest
 * (up to 2) that fits the line with 32 pixels of margin on each side, so a wide
 * board shows large letters and a small one keeps them inside the strip. */
static void sim_silk_text(SDL_Surface *surface, int board_w, int y, const char *text, Uint32 color)
{
    int units = sim_silk_units(text);
    if (units == 0) {
        return;
    }
    int scale = SDL_min((board_w - 64) / units, 2);
    if (scale < 1) {
        scale = 1;
    }
    sim_silk_draw(surface, (board_w - units * scale) / 2, y, text, color, scale);
}

/* Write a short label centered on cx at a fixed scale (the component mark). */
static void sim_silk_label(SDL_Surface *surface, int cx, int y, const char *text, Uint32 color, int scale)
{
    sim_silk_draw(surface, cx - sim_silk_units(text) * scale / 2, y, text, color, scale);
}

/* Pre-render the development board into a static surface, once per preview
 * window, from the fixed look above. The board never changes while the
 * application runs, so it is uploaded to a single texture at creation and
 * only the LCD texture is refreshed afterwards.
 *
 * The screen cut-out is `panel_w` x `panel_h` logical pixels at (screen_x,
 * screen_y); that is the exact rectangle the LCD texture is composited into.
 * Keeping it 1:1 means the preview shows the LCD at the resolution the
 * program configured, no matter how large the surrounding board is. */
static SDL_Surface *sim_board_render(int panel_w, int panel_h, int *out_screen_x, int *out_screen_y)
{
    int board_w = panel_w + 2 * SIM_BOARD_MARGIN_X;
    int board_h = panel_h + SIM_BOARD_MARGIN_TOP + SIM_BOARD_MARGIN_BOTTOM;
    int screen_x = SIM_BOARD_MARGIN_X;
    int screen_y = SIM_BOARD_MARGIN_TOP;

    SDL_Surface *s = SDL_CreateSurface(board_w, board_h, SDL_PIXELFORMAT_RGBA32);
    if (!s) {
        return NULL;
    }
    /* The surface starts transparent; the shadow and the LED glow blend, and
     * the rounded corners stay transparent so the preview backdrop shows. */
    SDL_SetSurfaceBlendMode(s, SDL_BLENDMODE_BLEND);

    Uint32 pcb = sim_board_color(s, SIM_BOARD_PCB);
    Uint32 pcb_edge = sim_board_color(s, SIM_BOARD_PCB_EDGE);
    Uint32 bezel = sim_board_color(s, SIM_BOARD_BEZEL);
    Uint32 bezel_hi = sim_board_color(s, SIM_BOARD_BEZEL_HI);
    Uint32 screen_bg = sim_board_color(s, SIM_BOARD_SCREEN);
    Uint32 silk = sim_board_color(s, SIM_BOARD_SILK);
    Uint32 gold = sim_board_color(s, SIM_BOARD_GOLD);
    Uint32 metal = sim_board_color(s, SIM_BOARD_METAL);
    Uint32 dark = sim_board_color(s, SIM_BOARD_DARK);
    Uint32 led = sim_board_color(s, SIM_BOARD_LED);
    Uint32 hole_bg = sim_board_color(s, SIM_BOARD_BACKGROUND);

    /* The PCB slab, inset so the drop shadow fits on the bottom and right. */
    int shadow = SIM_BOARD_SHADOW_OFFSET;
    int pad = shadow > 0 ? shadow + 3 : 3;
    int pcb_x = 3;
    int pcb_y = 3;
    int pcb_w = board_w - pcb_x - pad;
    int pcb_h = board_h - pcb_y - pad;
    int radius = SIM_BOARD_CORNER_RADIUS;
    sim_board_shadow(s, pcb_x, pcb_y, pcb_w, pcb_h, radius, shadow);
    sim_board_rounded_rect(s, pcb_x, pcb_y, pcb_w, pcb_h, radius, pcb_edge);
    sim_board_rounded_rect(s, pcb_x + 2, pcb_y + 2, pcb_w - 4, pcb_h - 4, radius > 2 ? radius - 2 : 0, pcb);

    /* Display module: bezel with a top highlight, then the screen cut-out the
     * LCD texture is composited exactly onto. */
    int module_x = screen_x - 12;
    int module_y = screen_y - 12;
    int module_w = panel_w + 24;
    int module_h = panel_h + 24;
    sim_board_rounded_rect(s, module_x, module_y, module_w, module_h, 3, bezel_hi);
    sim_board_rounded_rect(s, module_x + 2, module_y + 2, module_w - 4, module_h - 4, 2, bezel);
    sim_board_fill(s, screen_x - 2, screen_y - 2, panel_w + 4, panel_h + 4, silk);
    sim_board_fill(s, screen_x, screen_y, panel_w, panel_h, screen_bg);

    /* Top strip: the power LED centered above the silkscreen name. The name
     * baseline clears the corner mounting holes, and even the largest letters
     * (scale 2, 14 pixels tall) stay above the bezel that starts at
     * screen_y - 12. */
    int led_x = board_w / 2 - 8;
    int led_y = 12;
    sim_board_rounded_rect(s, led_x - 3, led_y - 3, 22, 14, 5, sim_board_rgba(s, SIM_BOARD_LED, 60));
    sim_board_fill(s, led_x, led_y, 16, 8, dark);
    sim_board_fill(s, led_x + 2, led_y + 2, 12, 4, led);
    sim_silk_text(s, board_w, 28, SIM_BOARD_NAME, silk);

    /* Gold accents on the bezel, mirrored above and below the screen: the
     * top one leaves from the left edge, the bottom one returns to the right
     * edge, like the flex cable mark of a display module. */
    int accent_w = (int)((float)panel_w * 0.35f);
    sim_board_fill(s, screen_x, screen_y - 10, accent_w, 3, gold);
    int screen_bottom = screen_y + panel_h;
    sim_board_fill(s, screen_x + panel_w - accent_w, screen_bottom + 7, accent_w, 3, gold);

    /* Bottom strip: a small component mark centered under the screen, the
     * counterpart of the name above it, so the board reads balanced instead
     * of top-heavy. */
    sim_silk_label(s, board_w / 2, screen_bottom + 28, SIM_BOARD_MARK, silk, 1);

    /* Mounting holes in the four corners: a metal ring around a dark center. */
    int hole = 8;
    int hole_inset = 16;
    int holes[4][2] = {
        {pcb_x + hole_inset, pcb_y + hole_inset},
        {pcb_x + pcb_w - hole_inset - hole, pcb_y + hole_inset},
        {pcb_x + hole_inset, pcb_y + pcb_h - hole_inset - hole},
        {pcb_x + pcb_w - hole_inset - hole, pcb_y + pcb_h - hole_inset - hole},
    };
    for (int i = 0; i < 4; i++) {
        sim_board_fill(s, holes[i][0] - 1, holes[i][1] - 1, hole + 2, hole + 2, metal);
        sim_board_fill(s, holes[i][0], holes[i][1], hole, hole, hole_bg);
    }

    *out_screen_x = screen_x;
    *out_screen_y = screen_y;
    return s;
}

/* One video subsystem reference for the whole component, released by
 * esp_lcd_sim_sdl_backend_deinit(). Failing here is the headless case: no
 * video driver can be started (including SDL_VIDEODRIVER pointing at a
 * driver that does not exist), so the caller falls back to a framebuffer-only
 * panel. */
static esp_err_t sim_sdl_ensure_init(void)
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

esp_err_t esp_lcd_sim_sdl_backend_create(const esp_lcd_sim_config_t *config, uint8_t *framebuffer,
                                          size_t framebuffer_size, uint32_t sdl_format, size_t bytes_per_pixel,
                                          esp_lcd_sim_backend_cb_t screenshot_cb, void *screenshot_ctx,
                                          void **ret_ctx)
{
    sim_sdl_ctx_t *ctx = NULL;
    int scale = config->scale > 0 ? config->scale : 1;

    *ret_ctx = NULL;
    if (!config->create_window) {
        /* Framebuffer only: the content can still be exported as a PNG file,
         * no SDL object needed. There is no window to draw a board on, and no
         * window to receive the screenshot key either. */
        return ESP_OK;
    }

    if (sim_sdl_ensure_init() != ESP_OK) {
        /* Headless machine (or SDL_VIDEODRIVER pointing at a driver that does
         * not exist): a preview is a convenience, not a requirement. Keep the
         * panel framebuffer-only; it still renders and exports PNGs. */
        ESP_LOGW(TAG, "no display available, continuing without a preview window");
        return ESP_OK;
    }

    ctx = calloc(1, sizeof(sim_sdl_ctx_t));
    ESP_RETURN_ON_FALSE(ctx, ESP_ERR_NO_MEM, TAG, "no mem for SDL backend");
    ctx->framebuffer = framebuffer;
    ctx->framebuffer_size = framebuffer_size;
    ctx->bytes_per_pixel = bytes_per_pixel;
    ctx->width = config->width;
    ctx->height = config->height;

    /* Decide the logical scene size up front. With the board frame the whole
     * board is the logical scene and the LCD is composited into its screen
     * cut-out; without it the LCD alone is the scene. The LCD texture is always
     * exactly width x height, so one panel pixel stays one logical pixel. */
    int logical_w = config->width;
    int logical_h = config->height;
    ctx->screen_rect.x = 0.0f;
    ctx->screen_rect.y = 0.0f;
    ctx->screen_rect.w = (float)config->width;
    ctx->screen_rect.h = (float)config->height;
    SDL_Surface *board_surface = NULL;
    if (config->show_board_frame) {
        int screen_x = 0;
        int screen_y = 0;
        int board_w = config->width + 2 * SIM_BOARD_MARGIN_X;
        int board_h = config->height + SIM_BOARD_MARGIN_TOP + SIM_BOARD_MARGIN_BOTTOM;
        /* The board scene is larger than the panel, so the window size guard
         * in esp_lcd_new_panel_sim_sdl() (panel * scale) does not cover it.
         * A board is only decoration: skip it rather than overflow the window
         * dimensions, and keep the already validated panel-only scene. */
        if ((int64_t)board_w * scale <= INT_MAX && (int64_t)board_h * scale <= INT_MAX) {
            board_surface = sim_board_render(config->width, config->height, &screen_x, &screen_y);
        }
        if (board_surface) {
            logical_w = board_surface->w;
            logical_h = board_surface->h;
            ctx->screen_rect.x = (float)screen_x;
            ctx->screen_rect.y = (float)screen_y;
            ctx->screen_rect.w = (float)config->width;
            ctx->screen_rect.h = (float)config->height;
        } else {
            ESP_LOGW(TAG, "board frame skipped, showing the LCD only");
        }
    }

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
            SDL_DestroySurface(board_surface);
            free(ctx);
            return ESP_ERR_NO_MEM;
        }
    }
    /* The screenshot key and its handler: the panel installs the function to
     * call once the key is pressed in this window. */
    ctx->screenshot_key = config->screenshot_key;
    ctx->screenshot_fn = screenshot_cb;
    ctx->screenshot_ctx = screenshot_ctx;

    /* Keep windows resizable so the preview can be zoomed by the user as well.
     * The initial size is the whole logical scene times the requested scale. */
    ctx->window = SDL_CreateWindow(config->window_title ? config->window_title : SIM_DEFAULT_WINDOW_TITLE,
                                   logical_w * scale, logical_h * scale, SDL_WINDOW_RESIZABLE);
    if (!ctx->window) {
        ESP_LOGE(TAG, "failed to create preview window: %s", SDL_GetError());
        goto fail;
    }
    if (!SDL_SetWindowMinimumSize(ctx->window, logical_w, logical_h)) {
        ESP_LOGW(TAG, "failed to set window minimum size: %s", SDL_GetError());
    }
    /* Keep the window shape close to the scene shape while the user resizes. */
    if (!SDL_SetWindowAspectRatio(ctx->window, (float)logical_w / (float)logical_h,
                                  (float)logical_w / (float)logical_h)) {
        ESP_LOGW(TAG, "failed to set window aspect ratio: %s", SDL_GetError());
    }

    /* Let SDL pick the renderer. Naming "software" would force SDL's
     * texture-framebuffer emulation on Wayland: software rasterization plus a
     * full-window re-upload on every present, defeating the dirty-rect upload
     * below. SDL still falls back to software if no accelerated driver starts. */
    ctx->renderer = SDL_CreateRenderer(ctx->window, NULL);
    if (!ctx->renderer) {
        ESP_LOGE(TAG, "failed to create preview renderer: %s", SDL_GetError());
        goto fail;
    }
    /* Force vsync off: an occluded window never gets the compositor's frame
     * callback, which would stall the pump loop. The application paces it. */
    if (!SDL_SetRenderVSync(ctx->renderer, SDL_RENDERER_VSYNC_DISABLED)) {
        ESP_LOGW(TAG, "failed to disable renderer vsync: %s", SDL_GetError());
    }
    /* The whole logical scene (board + screen, or just the LCD) is scaled to
     * the window; SDL_RenderPresent() of a dirty frame is all the preview
     * needs. Letterboxing keeps the pixels square at any window size, and the
     * bars around a window whose shape does not match the scene show the
     * render draw color, so make it the board backdrop instead of SDL's
     * default white: the preview then blends into the scene, and a window
     * resized by the compositor never flashes an empty white margin. */
    if (!SDL_SetRenderDrawColor(ctx->renderer, (Uint8)(SIM_BOARD_BACKGROUND >> 16), (Uint8)(SIM_BOARD_BACKGROUND >> 8),
                                (Uint8)SIM_BOARD_BACKGROUND, 255)) {
        ESP_LOGW(TAG, "failed to set the preview background color: %s", SDL_GetError());
    }
    if (!SDL_SetRenderLogicalPresentation(ctx->renderer, logical_w, logical_h,
                                          SDL_LOGICAL_PRESENTATION_LETTERBOX)) {
        ESP_LOGW(TAG, "failed to set the preview scaling: %s", SDL_GetError());
    }
    /* The size the first present will be made for; pump() re-presents whenever
     * the window moves away from it. Seeding it here keeps an idle panel from
     * presenting before the GUI has drawn anything into the texture. */
    if (!SDL_GetWindowSizeInPixels(ctx->window, &ctx->presented_pixel_w, &ctx->presented_pixel_h)) {
        ctx->presented_pixel_w = logical_w * scale;
        ctx->presented_pixel_h = logical_h * scale;
    }

    /* Upload the static board once. Failing here is not fatal: the logical
     * scene already uses the board size, so the LCD simply appears centered on
     * a plain background instead of on the drawn board. */
    if (board_surface) {
        ctx->board_texture = SDL_CreateTextureFromSurface(ctx->renderer, board_surface);
        if (!ctx->board_texture) {
            ESP_LOGW(TAG, "failed to upload the board frame: %s, showing the LCD only", SDL_GetError());
        } else if (!SDL_SetTextureBlendMode(ctx->board_texture, SDL_BLENDMODE_BLEND)) {
            ESP_LOGW(TAG, "failed to set the board blend mode: %s", SDL_GetError());
        }
        SDL_DestroySurface(board_surface);
        board_surface = NULL;
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
    ESP_LOGI(TAG, "preview window '%s' created (%dx%d, scale %d, %s)",
             config->window_title ? config->window_title : SIM_DEFAULT_WINDOW_TITLE,
             logical_w * scale, logical_h * scale, scale,
             ctx->board_texture ? "with board frame" : "LCD only");
    *ret_ctx = ctx;
    return ESP_OK;

fail:
    if (ctx->board_texture) {
        SDL_DestroyTexture(ctx->board_texture);
    }
    SDL_DestroySurface(board_surface);
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
    /* The window, renderer or texture could not be created. Fall back to a
     * framebuffer-only panel instead of failing the panel creation: the
     * framebuffer still renders and exports PNGs. */
    ESP_LOGW(TAG, "preview window unavailable, continuing without it");
    return ESP_OK;
}

/* Drop every SDL resource of one backend. */
static void sim_sdl_free_objects(sim_sdl_ctx_t *ctx)
{
    if (ctx->board_texture) {
        SDL_DestroyTexture(ctx->board_texture);
        ctx->board_texture = NULL;
    }
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
    ctx->dirty_count = 0;
}

/* Close the window of one backend after a close request, keeping the rest of
 * the backend alive: the panel framebuffer still renders and exports PNGs. */
static void sim_sdl_close_backend(sim_sdl_ctx_t *ctx)
{
    sim_sdl_free_objects(ctx);
    /* The window is gone, so it can no longer receive the screenshot key.
     * Leaving the callback installed would let a stray event match this
     * backend (see sim_sdl_find_screenshot_owner()). */
    ctx->screenshot_fn = NULL;
    ctx->screenshot_ctx = NULL;
    ESP_LOGI(TAG, "preview window closed by the user");
}

void esp_lcd_sim_sdl_backend_delete(void *backend)
{
    sim_sdl_ctx_t *ctx = backend;
    if (!ctx) {
        return;
    }
    sim_sdl_ctx_t **link = &s_backends;
    while (*link && *link != ctx) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = ctx->next;
    }
    sim_sdl_free_objects(ctx);
    free(ctx);
}

/* Union of two regions. */
static SDL_Rect sim_sdl_rect_union(const SDL_Rect *a, const SDL_Rect *b)
{
    int left = SDL_min(a->x, b->x);
    int top = SDL_min(a->y, b->y);
    int right = SDL_max(a->x + a->w, b->x + b->w);
    int bottom = SDL_max(a->y + a->h, b->y + b->h);
    SDL_Rect merged = {left, top, right - left, bottom - top};
    return merged;
}

/* True when two regions overlap or share an edge (corner contact included):
 * one upload of the union then moves no more pixels than two separate
 * uploads, while saving the per-call overhead. */
static bool sim_sdl_rects_touch(const SDL_Rect *a, const SDL_Rect *b)
{
    return a->x <= b->x + b->w && b->x <= a->x + a->w
           && a->y <= b->y + b->h && b->y <= a->y + a->h;
}

/* Clip the region to the panel and record it as pending. Regions that touch
 * merge, so a GUI flushing adjacent bands uploads them as one; scattered
 * regions stay separate, so a few small flushes do not upload the whole frame
 * the way a single union rect would. */
static void sim_sdl_mark_dirty_rect(sim_sdl_ctx_t *ctx, int x, int y, int width, int height)
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
    SDL_Rect rect = {x, y, right - x, bottom - y};

    /* Absorb every pending region the new one touches. The union can reach
     * further regions in turn, so rescan until nothing merges anymore. */
    bool merged = true;
    while (merged) {
        merged = false;
        for (int i = 0; i < ctx->dirty_count; i++) {
            if (sim_sdl_rects_touch(&ctx->dirty_rects[i], &rect)) {
                rect = sim_sdl_rect_union(&ctx->dirty_rects[i], &rect);
                ctx->dirty_rects[i] = ctx->dirty_rects[--ctx->dirty_count];
                merged = true;
                break;
            }
        }
    }

    if (ctx->dirty_count < SIM_DIRTY_RECTS_MAX) {
        ctx->dirty_rects[ctx->dirty_count++] = rect;
        return;
    }
    /* The list is full: merge into the region that grows the least. More
     * flushes than the list holds degrade to larger unions, never to dropped
     * pixels. */
    int best = 0;
    int64_t best_area = INT64_MAX;
    for (int i = 0; i < ctx->dirty_count; i++) {
        SDL_Rect grown = sim_sdl_rect_union(&ctx->dirty_rects[i], &rect);
        int64_t area = (int64_t)grown.w * grown.h;
        if (area < best_area) {
            best_area = area;
            best = i;
        }
    }
    ctx->dirty_rects[best] = sim_sdl_rect_union(&ctx->dirty_rects[best], &rect);
}

/* Push the whole panel: the full-panel special case of the region push. */
esp_err_t esp_lcd_sim_sdl_backend_update(void *backend, const uint8_t *framebuffer, size_t framebuffer_size)
{
    sim_sdl_ctx_t *ctx = backend;
    if (!ctx) {
        return ESP_OK;
    }
    return esp_lcd_sim_sdl_backend_update_rect(backend, framebuffer, framebuffer_size,
                                               0, 0, ctx->width, ctx->height);
}

esp_err_t esp_lcd_sim_sdl_backend_update_rect(void *backend, const uint8_t *framebuffer, size_t framebuffer_size,
                                               int x, int y, int width, int height)
{
    sim_sdl_ctx_t *ctx = backend;
    if (!ctx) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(framebuffer == ctx->framebuffer && framebuffer_size == ctx->framebuffer_size,
                        ESP_ERR_INVALID_ARG, TAG, "framebuffer mismatch");
    sim_sdl_mark_dirty_rect(ctx, x, y, width, height);
    return ESP_OK;
}

/* Sample the pointer from SDL's cache (refreshed by the pump loop) without
 * touching the event queue. Window pixels map through the logical presentation
 * into panel pixels; only the screen cut-out is the simulated glass. */
esp_err_t esp_lcd_sim_sdl_backend_mouse_get(void *backend, esp_lcd_sim_mouse_data_t *data)
{
    sim_sdl_ctx_t *ctx = backend;
    data->pressed = false;
    if (!ctx) {
        /* Framebuffer-only panel: no pointer ever reaches it. */
        data->x = 0;
        data->y = 0;
        return ESP_OK;
    }
    if (!ctx->window || !ctx->renderer) {
        /* The window was closed: released at the last position sampled on
         * the screen, (0,0) if the pointer never was there. */
        data->x = ctx->mouse_x;
        data->y = ctx->mouse_y;
        return ESP_OK;
    }
    if (SDL_GetMouseFocus() != ctx->window) {
        data->x = ctx->mouse_x;
        data->y = ctx->mouse_y;
        return ESP_OK;
    }
    float window_x = 0.0f;
    float window_y = 0.0f;
    SDL_MouseButtonFlags buttons = SDL_GetMouseState(&window_x, &window_y);
    float logical_x = 0.0f;
    float logical_y = 0.0f;
    if (!SDL_RenderCoordinatesFromWindow(ctx->renderer, window_x, window_y, &logical_x, &logical_y)) {
        data->x = ctx->mouse_x;
        data->y = ctx->mouse_y;
        return ESP_OK;
    }
    float panel_x = logical_x - ctx->screen_rect.x;
    float panel_y = logical_y - ctx->screen_rect.y;
    if (panel_x >= 0.0f && panel_y >= 0.0f
        && panel_x < (float)ctx->width && panel_y < (float)ctx->height) {
        /* On the glass: remember the position for the hover reports that
         * follow once the pointer moves back onto the board. */
        ctx->mouse_x = (int)panel_x;
        ctx->mouse_y = (int)panel_y;
        data->pressed = (buttons & SDL_BUTTON_MASK(SDL_BUTTON_LEFT)) != 0;
    }
    data->x = ctx->mouse_x;
    data->y = ctx->mouse_y;
    return ESP_OK;
}

/* Compare a pressed key against the configured one. ASCII letters match with
 * or without Shift on both sides: the configured letter may be upper or lower
 * case, and the pressed key arrives with the Shift state baked in. Every
 * other key has to match exactly. */
static bool sim_sdl_key_matches(int configured, SDL_Keycode pressed)
{
    if (configured <= 0) {
        return false;
    }
    if (configured >= 'A' && configured <= 'Z') {
        configured += 'a' - 'A';
    }
    if (pressed >= 'A' && pressed <= 'Z') {
        pressed += 'a' - 'A';
    }
    return configured == (int)pressed;
}

/* The screenshot key belongs to the window it was pressed in, so that several
 * panels with different keys do not react to each other. Returns the backend
 * that has to save, or NULL. */
static sim_sdl_ctx_t *sim_sdl_find_screenshot_owner(SDL_Window *window, SDL_Keycode key)
{
    for (sim_sdl_ctx_t *ctx = s_backends; ctx; ctx = ctx->next) {
        /* A closed backend has a NULL window. SDL_GetWindowFromID() also
         * returns NULL for an unknown window id, and comparing two NULLs
         * would make such an event trigger a screenshot on a dead window. */
        if (ctx->window && ctx->window == window && ctx->screenshot_fn
            && sim_sdl_key_matches(ctx->screenshot_key, key)) {
            return ctx;
        }
    }
    return NULL;
}

/* Byte-swap one dirty region of a framebuffer whose byte order differs from
 * the host into the scratch copy, so the native-endian upload reads swapped
 * pixels. Loading 16-bit pixels and swapping them (a single rotate on most
 * hosts) vectorizes, unlike assembling each pixel from two byte loads. */
static void sim_sdl_byteswap_rect(sim_sdl_ctx_t *ctx, const SDL_Rect *rect)
{
    const uint8_t *src = ctx->framebuffer + ((size_t)rect->y * ctx->width + rect->x) * 2;
    uint16_t *dst = ctx->scratch + (size_t)rect->y * ctx->width + rect->x;
    for (int y = 0; y < rect->h; y++) {
        for (int x = 0; x < rect->w; x++) {
            uint16_t pixel;
            memcpy(&pixel, src + (size_t)x * 2, sizeof(pixel));
            dst[x] = SDL_Swap16(pixel);
        }
        src += (size_t)ctx->width * 2;
        dst += ctx->width;
    }
}

static void sim_sdl_handle_event(const SDL_Event *event)
{
    if (event->type == SDL_EVENT_QUIT) {
        s_window_close_requested = true;
    } else if (event->type == SDL_EVENT_KEY_DOWN) {
        /* A held-down key sends repeated key-down events; one press saves
         * one screenshot. */
        if (event->key.repeat) {
            return;
        }
        sim_sdl_ctx_t *owner = sim_sdl_find_screenshot_owner(
            SDL_GetWindowFromID(event->key.windowID), event->key.key);
        if (owner) {
            owner->screenshot_fn(owner->screenshot_ctx);
        }
    } else if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        SDL_Window *closed = SDL_GetWindowFromID(event->window.windowID);
        for (sim_sdl_ctx_t *ctx = s_backends; ctx; ctx = ctx->next) {
            /* A closed backend has a NULL window; do not match it against a
             * NULL window id from SDL_GetWindowFromID(). */
            if (ctx->window && ctx->window == closed) {
                /* SDL delivers a close request instead of destroying the
                 * window: an unread one would leave a dead window on screen. */
                sim_sdl_close_backend(ctx);
                s_window_close_requested = true;
                break;
            }
        }
    }
    /* Only the screenshot key and the window lifecycle are acted on; every
     * other event, mouse motion included, is drained and ignored. */
}

/* Empty the whole queue, not just the events acted on: the event-driven wait
 * in esp_lcd_sim_wait_events() returns at once while any event is still
 * queued. Pointer input is sampled separately through SDL_GetMouseState(), so
 * discarding the mouse events here loses nothing. */
static void sim_sdl_drain_events(void)
{
    SDL_PumpEvents();
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        sim_sdl_handle_event(&event);
    }
}

esp_err_t esp_lcd_sim_sdl_backend_pump(void)
{
    /* esp_lcd_sim_pump_events() has no panel handle, so it walks the registry;
     * it can also be called before the first panel exists. A framebuffer-only
     * build never initializes SDL, so there is no queue to drain there. */
    if (s_sdl_initialized) {
        sim_sdl_drain_events();
    }

    /* Present every dirty window. The renderer is created without vsync (see
     * esp_lcd_sim_sdl_backend_create()), so an idle GUI is not throttled
     * here either; the application paces the pump loop. A headless build has
     * no window and nothing to present. */
    for (sim_sdl_ctx_t *ctx = s_backends; ctx; ctx = ctx->next) {
        /* Present when the framebuffer changed, but also when the window no
         * longer has the size the last present was made for: the renderer
         * letterboxes the logical scene into the window, so a resize changes
         * what belongs on screen even though the panel framebuffer did not.
         * This is what a compositor-initiated resize needs: on Wayland the
         * window only takes its new size once the client commits a buffer of
         * that size, and a static GUI that stopped presenting after its first
         * frame would otherwise stay parked in the corner of the enlarged
         * window with the rest of it never painted. */
        bool resized = false;
        int pixel_w = ctx->presented_pixel_w;
        int pixel_h = ctx->presented_pixel_h;
        if (ctx->window && SDL_GetWindowSizeInPixels(ctx->window, &pixel_w, &pixel_h)) {
            resized = pixel_w != ctx->presented_pixel_w || pixel_h != ctx->presented_pixel_h;
        }
        if ((ctx->dirty_count == 0 && !resized) || !ctx->renderer) {
            continue;
        }
        /* Upload each drawn region on its own: the pitch still describes the
         * whole framebuffer row, the pixel pointer just starts at the first
         * row and column of the region. A failed upload stays in the list so
         * the next pump retries it instead of silently dropping the frame. */
        bool uploaded = true;
        int failed = 0;
        for (int i = 0; i < ctx->dirty_count; i++) {
            const SDL_Rect *rect = &ctx->dirty_rects[i];
            if (ctx->byteswap) {
                sim_sdl_byteswap_rect(ctx, rect);
            }
            const uint8_t *src = (ctx->byteswap ? (const uint8_t *)ctx->scratch : ctx->framebuffer)
                                 + ((size_t)rect->y * ctx->width + rect->x) * ctx->bytes_per_pixel;
            if (!SDL_UpdateTexture(ctx->texture, rect, src,
                                   (int)((size_t)ctx->width * ctx->bytes_per_pixel))) {
                ESP_LOGE(TAG, "failed to upload the frame: %s", SDL_GetError());
                ctx->dirty_rects[failed++] = *rect;
                uploaded = false;
            }
        }
        ctx->dirty_count = failed;
        if (uploaded) {
            /* Composite the preview: the static board fills the logical
             * scene, then the LCD is drawn into its screen cut-out. Without a
             * board the screen rect is the whole scene, so this is the
             * original full-window LCD view. The board is never written into
             * the panel framebuffer, so the PNG export stays LCD-only. */
            bool presented = SDL_RenderClear(ctx->renderer);
            if (presented && ctx->board_texture) {
                presented = SDL_RenderTexture(ctx->renderer, ctx->board_texture, NULL, NULL);
            }
            if (presented) {
                presented = SDL_RenderTexture(ctx->renderer, ctx->texture, NULL, &ctx->screen_rect);
            }
            if (presented) {
                presented = SDL_RenderPresent(ctx->renderer);
            }
            if (presented) {
                /* Only a successful present retires the size: a failed one is
                 * retried by the next pump, which still sees `resized`. */
                ctx->presented_pixel_w = pixel_w;
                ctx->presented_pixel_h = pixel_h;
            } else {
                ESP_LOGE(TAG, "failed to present the frame: %s", SDL_GetError());
            }
        }
    }
    return ESP_OK;
}

esp_err_t esp_lcd_sim_sdl_backend_wait_events(uint32_t timeout_ms)
{
    /* A framebuffer-only build never initializes SDL and has no queue to wait
     * on. SDL_WaitEventTimeout() takes a signed timeout, so clamp the unsigned
     * one; passing NULL leaves the queued events for the next pump. */
    if (s_sdl_initialized) {
        Sint32 timeout = timeout_ms > INT32_MAX ? INT32_MAX : (Sint32)timeout_ms;
        SDL_WaitEventTimeout(NULL, timeout);
    }
    return ESP_OK;
}

bool esp_lcd_sim_window_close_requested(void)
{
    return s_window_close_requested;
}

bool esp_lcd_sim_sdl_backend_window_is_open(void *backend)
{
    sim_sdl_ctx_t *ctx = backend;
    return ctx != NULL && ctx->window != NULL;
}

esp_err_t esp_lcd_sim_sdl_backend_deinit(void)
{
    /* Every backend still holds SDL objects until its panel is deleted.
     * Quitting SDL first would leave those objects pointing at a torn-down
     * subsystem, so refuse instead of silently invalidating live panels. */
    ESP_RETURN_ON_FALSE(s_backends == NULL, ESP_ERR_INVALID_STATE, TAG,
                        "delete all sim panels before deinitializing");
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
