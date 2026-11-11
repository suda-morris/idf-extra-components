/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * esp_console command interface of the simulated panel. The command names and
 * their arguments are part of the public API (documented in esp_lcd_sim.h);
 * this file owns their parsing and implementation, so an application only has
 * to register them and let esp_console dispatch the lines.
 *
 * The commands are meant to run on a background REPL task (the application
 * creates one with esp_console_new_repl_stdio() + esp_console_start_repl())
 * while the GUI renders on its own thread. They only call the thread-safe
 * panel entry points (inject_touch, inject_reset, screenshot_save_png) and
 * never SDL, so they need no GUI-thread affinity. 'touch' and 'wait' sleep for
 * the requested time so the independent GUI thread can sample the input and
 * advance; the component does not know the GUI library and cannot drive it.
 */
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_lcd_sim.h"

#if CONFIG_ESP_LCD_SIM_CONSOLE

#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_console.h"
#include "esp_log.h"
#include "argtable3/argtable3.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lcd_sim.console";

/* GUI run time after an injected touch when the config does not pick one. */
#define SIM_CONSOLE_TOUCH_SETTLE_MS 100
/* Upper bound for 'wait' so a stray argument cannot block the loop for long. */
#define SIM_CONSOLE_WAIT_MAX_MS     10000

/* The command set drives one panel. The config is copied here so the caller's
 * structure can be a temporary; the quit_requested pointer it holds must
 * outlive the registration. */
static esp_lcd_sim_console_config_t s_console;
static bool s_registered;

static struct {
    struct arg_int *x;
    struct arg_int *y;
    struct arg_int *pressed;
    struct arg_end *end;
} s_touch_args;

static struct {
    struct arg_int *ms;
    struct arg_end *end;
} s_wait_args;

static struct {
    struct arg_str *path;
    struct arg_end *end;
} s_frame_args;

static int sim_console_touch(void *ctx, int argc, char **argv)
{
    esp_lcd_sim_console_config_t *cfg = ctx;
    if (arg_parse(argc, argv, (void **)&s_touch_args) != 0) {
        arg_print_errors(stdout, s_touch_args.end, argv[0]);
        return 1;
    }
    int x = s_touch_args.x->ival[0];
    int y = s_touch_args.y->ival[0];
    int pressed = s_touch_args.pressed->ival[0];
    if (pressed != 0 && pressed != 1) {
        ESP_LOGE(TAG, "touch: <0|1> must be 0 (release) or 1 (press)");
        return 1;
    }
    esp_err_t err = esp_lcd_sim_input_inject_touch(cfg->panel, x, y, pressed != 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch: %s", esp_err_to_name(err));
        return 1;
    }
    ESP_LOGI(TAG, "injected touch (%d, %d) %s", x, y, pressed ? "pressed" : "released");
    /* The GUI runs on its own thread; sleep so it samples the new state and
     * finishes any click/scroll gesture before the next command runs. */
    uint32_t settle = cfg->touch_settle_ms ? cfg->touch_settle_ms : SIM_CONSOLE_TOUCH_SETTLE_MS;
    vTaskDelay(pdMS_TO_TICKS(settle));
    return 0;
}

static int sim_console_reset(void *ctx, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_lcd_sim_console_config_t *cfg = ctx;
    esp_err_t err = esp_lcd_sim_input_inject_reset(cfg->panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reset: %s", esp_err_to_name(err));
        return 1;
    }
    ESP_LOGI(TAG, "touch injection reset");
    return 0;
}

static int sim_console_frame(void *ctx, int argc, char **argv)
{
    esp_lcd_sim_console_config_t *cfg = ctx;
    if (arg_parse(argc, argv, (void **)&s_frame_args) != 0) {
        arg_print_errors(stdout, s_frame_args.end, argv[0]);
        return 1;
    }
    esp_err_t err = esp_lcd_sim_screenshot_save_png(cfg->panel, s_frame_args.path->sval[0]);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "frame: %s", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

static int sim_console_wait(void *ctx, int argc, char **argv)
{
    (void)ctx;   /* wait only needs the wall time, not the panel */
    if (arg_parse(argc, argv, (void **)&s_wait_args) != 0) {
        arg_print_errors(stdout, s_wait_args.end, argv[0]);
        return 1;
    }
    int ms = s_wait_args.ms->ival[0];
    ms = ms < 0 ? 0 : (ms > SIM_CONSOLE_WAIT_MAX_MS ? SIM_CONSOLE_WAIT_MAX_MS : ms);
    /* Let the independent GUI thread run for the requested wall time. */
    vTaskDelay(pdMS_TO_TICKS((uint32_t)ms));
    ESP_LOGI(TAG, "waited %d ms", ms);
    return 0;
}

static int sim_console_quit(void *ctx, int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_lcd_sim_console_config_t *cfg = ctx;
    if (cfg->quit_requested) {
        *cfg->quit_requested = true;
    }
    return 0;
}

esp_err_t esp_lcd_sim_console_register(const esp_lcd_sim_console_config_t *config)
{
    if (config == NULL || config->panel == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_registered) {
        return ESP_ERR_INVALID_STATE;
    }
    s_console = *config;

    s_touch_args.x = arg_int1(NULL, NULL, "<x>", "panel x coordinate");
    s_touch_args.y = arg_int1(NULL, NULL, "<y>", "panel y coordinate");
    s_touch_args.pressed = arg_int1(NULL, NULL, "<0|1>", "1 = press, 0 = release");
    s_touch_args.end = arg_end(2);
    const esp_console_cmd_t touch_cmd = {
        .command = "touch",
        .help = "Inject a pointer state and let the GUI settle",
        .func_w_context = &sim_console_touch,
        .argtable = &s_touch_args,
        .context = &s_console,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&touch_cmd), TAG, "register touch");

    const esp_console_cmd_t reset_cmd = {
        .command = "reset",
        .help = "Stop injecting touch and return to the real pointer",
        .func_w_context = &sim_console_reset,
        .context = &s_console,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&reset_cmd), TAG, "register reset");

    s_frame_args.path = arg_str1(NULL, NULL, "<path>", "output PNG file path");
    s_frame_args.end = arg_end(2);
    const esp_console_cmd_t frame_cmd = {
        .command = "frame",
        .help = "Export the current frame as a PNG",
        .func_w_context = &sim_console_frame,
        .argtable = &s_frame_args,
        .context = &s_console,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&frame_cmd), TAG, "register frame");

    s_wait_args.ms = arg_int1(NULL, NULL, "<ms>", "milliseconds to let the GUI run (max 10000)");
    s_wait_args.end = arg_end(2);
    const esp_console_cmd_t wait_cmd = {
        .command = "wait",
        .help = "Sleep while the GUI runs without input",
        .func_w_context = &sim_console_wait,
        .argtable = &s_wait_args,
        .context = &s_console,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&wait_cmd), TAG, "register wait");

    const esp_console_cmd_t quit_cmd = {
        .command = "quit",
        .help = "Request the application loop to stop",
        .func_w_context = &sim_console_quit,
        .context = &s_console,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&quit_cmd), TAG, "register quit");

    ESP_RETURN_ON_ERROR(esp_console_register_help_command(), TAG, "register help");
    s_registered = true;
    return ESP_OK;
}

#else /* !CONFIG_ESP_LCD_SIM_CONSOLE */

esp_err_t esp_lcd_sim_console_register(const esp_lcd_sim_console_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_ESP_LCD_SIM_CONSOLE */
