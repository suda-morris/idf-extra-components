idf_component_register(SRCS "src/esp_lcd_sim.c"
                            "src/esp_lcd_sim_screenshot.c"
                            "src/esp_lcd_sim_sdl_backend.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_lcd)

# Only build a static SDL3, and none of the SDL samples, tests or install rules.
set(SDL_STATIC ON)
set(SDL_SHARED OFF)
set(SDL_TEST_LIBRARY OFF)
set(SDL_TESTS OFF)
set(SDL_EXAMPLES OFF)
set(SDL_INSTALL OFF)

# Subsystems the panel does not use: Video opens the window, Render presents
# the frame buffer, everything else is dead weight.
set(_lcd_sim_disabled_subsystems AUDIO CAMERA JOYSTICK HAPTIC HIDAPI POWER
                                  SENSOR DIALOG TRAY GPU)
foreach(_lcd_sim_sub IN LISTS _lcd_sim_disabled_subsystems)
    set(SDL_${_lcd_sim_sub} OFF)
endforeach()

# Optional third party libraries SDL would otherwise pick up
set(SDL_SYSTEM_ICONV ON)
set(SDL_LIBICONV OFF)
set(SDL_FRIBIDI OFF)
set(SDL_LIBTHAI OFF)
set(SDL_DBUS OFF)
set(SDL_IBUS OFF)
set(SDL_LIBURING OFF)
set(SDL_LIBUDEV OFF)
# Audio is disabled; keep its option list honest so the configure summary does
# not depend on whether libpipewire-dev happens to be installed.
set(SDL_PIPEWIRE OFF)

# Video drivers that are tied to a board or to a GPU: off everywhere
set(SDL_RPI OFF)
set(SDL_ROCKCHIP OFF)
set(SDL_VIVANTE OFF)

# Wayland decorations: let SDL prefer the compositor's xdg-decoration and
# fall back to libdecor (loaded dynamically, so no build/runtime dependency is
# added when the library is missing). On GNOME/Weston this is what gives the
# preview window a title bar with a close button.
set(SDL_WAYLAND_LIBDECOR ON)

# Render drivers the panel does not need. The backend lets SDL pick its
# renderer, and with these off the only accelerated one left is OpenGL ES;
# software remains SDL's fallback. The disabled drivers would only add code
# and third-party dependencies to the application.
set(SDL_OPENGL OFF)
set(SDL_VULKAN OFF)
set(SDL_RENDER_GPU OFF)
set(SDL_RENDER_VULKAN OFF)

# SDL_OPENGLES must stay available: Wayland has no native window surface, so
# an accelerated renderer backed by OpenGL ES is what presents the preview
# window. With every accelerated render driver disabled SDL could still fall
# back to the software renderer, but on Wayland that path fails to obtain a
# window surface at all. The exported PNG never passes through SDL, so the
# renderer choice cannot affect it.

# Do not record dlopen dependencies in .note.dlopen sections: SDL is built into
# an ESP-IDF application, which does not read those sections.
set(SDL_DLOPEN_NOTES OFF)
# RPATH only matters to an installed shared SDL; we link SDL3 statically.
set(SDL_RPATH OFF)

# Allow the console build on Linux so a machine without the desktop packages
# can still build (the preview then falls back to a framebuffer-only panel).
set(SDL_UNIX_CONSOLE_BUILD ON)

# idf_component_register() seeds this directory's COMPILE_OPTIONS (with -Werror)
# and LINK_LIBRARIES (the common component deps), and the SDL subproject inherits
# both. Clear them just for add_subdirectory(): SDL then builds with its own
# warning set, and its unconditional export(TARGETS SDL3-static) does not pull
# IDF targets into the export set. Restore them afterwards for this component.
get_property(_lcd_sim_saved_compile_options DIRECTORY PROPERTY COMPILE_OPTIONS)
get_property(_lcd_sim_saved_link_libraries DIRECTORY PROPERTY LINK_LIBRARIES)
set_property(DIRECTORY PROPERTY COMPILE_OPTIONS "")
set_property(DIRECTORY PROPERTY LINK_LIBRARIES "")

add_subdirectory("SDL" EXCLUDE_FROM_ALL)

set_property(DIRECTORY PROPERTY COMPILE_OPTIONS ${_lcd_sim_saved_compile_options})
set_property(DIRECTORY PROPERTY LINK_LIBRARIES ${_lcd_sim_saved_link_libraries})
unset(_lcd_sim_saved_compile_options)
unset(_lcd_sim_saved_link_libraries)

target_link_libraries(${COMPONENT_LIB} PRIVATE SDL3::SDL3)
