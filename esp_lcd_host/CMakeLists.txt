idf_component_register(SRCS "src/esp_lcd_host.c"
                            "src/esp_lcd_host_screenshot.c"
                            "src/esp_lcd_host_sdl_backend.c"
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
set(_lcd_host_disabled_subsystems AUDIO CAMERA JOYSTICK HAPTIC HIDAPI POWER
                                  SENSOR DIALOG TRAY GPU)
foreach(_lcd_host_sub IN LISTS _lcd_host_disabled_subsystems)
    set(SDL_${_lcd_host_sub} OFF)
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

# Video drivers that are tied to a board or to a GPU: off everywhere
set(SDL_RPI OFF)
set(SDL_ROCKCHIP OFF)
set(SDL_VIVANTE OFF)

# The compositor draws the decorations through the xdg-decoration protocol instead of the libdecor library
set(SDL_WAYLAND_LIBDECOR OFF)

# Render drivers the panel never selects. The backend names the software
# renderer explicitly in SDL_CreateRenderer(), so the panel rasterizes the
# same way on every machine, whatever else SDL compiled in.
set(SDL_OPENGL OFF)
set(SDL_VULKAN OFF)
set(SDL_RENDER_GPU OFF)
set(SDL_RENDER_VULKAN OFF)

# Do not record dlopen dependencies in .note.dlopen sections: SDL is built into
# an ESP-IDF application, which does not read those sections.
set(SDL_DLOPEN_NOTES OFF)

# The console build is allowed on Linux so that a machine without the desktop packages can still build
set(SDL_UNIX_CONSOLE_BUILD ON)

# idf_component_register() seeds the directory scope of this directory with
# the build system settings, and the SDL subproject below inherits all of it.
# Two of those settings have to be taken back out, so that SDL configures and
# compiles as a standalone build of SDL would.
#
# The compile options: the build system compiles with -Werror and with a set of
# warnings that SDL does not pass, so SDL is built with the compiler defaults
# and its own warning set instead.
#
# The link libraries: SDL exports its library target unconditionally, and an
# exported target may not require a target outside the export set.
set_property(DIRECTORY PROPERTY COMPILE_OPTIONS "")
set_property(DIRECTORY PROPERTY LINK_LIBRARIES "")

add_subdirectory("SDL" EXCLUDE_FROM_ALL)

target_link_libraries(${COMPONENT_LIB} PRIVATE SDL3::SDL3)
