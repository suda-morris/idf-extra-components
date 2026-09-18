/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Definitions SDL expects from parts of itself that this port does not build.
 */
#include "SDL_internal.h"

#ifdef SDL_JOYSTICK_DISABLED
#include "joystick/SDL_gamepad_c.h"

/* The joystick subsystem is disabled in this port, but SDL_utils.c, which is
 * always compiled, asks for the gamepad type of a USB vendor/product pair when
 * it creates a device name. Without a joystick there is no controller database,
 * so the type is unknown. */
SDL_GamepadType SDL_GetGamepadTypeFromVIDPID(Uint16 vendor, Uint16 product, const char *name, bool forUI)
{
    (void)vendor;
    (void)product;
    (void)name;
    (void)forUI;
    return SDL_GAMEPAD_TYPE_UNKNOWN;
}
#endif
