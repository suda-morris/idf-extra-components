idf_component_register(SRCS "src/esp_lcd_host.c"
                            "src/esp_lcd_host_screenshot.c"
                            "src/esp_lcd_host_sdl_backend.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_lcd)

# Build the vendored SDL3 with the CMake project of SDL itself: the source
# lists, the platform probes and the generated SDL_build_config.h come from the
# submodule, so this file only turns off what an LCD panel does not need.
# Everything whose SDL default already matches this component (the X11
# extensions, the shared loading of the desktop libraries, the dummy and
# offscreen drivers, EGL/OpenGL ES for KMSDRM) is left to SDL.

set(SDL_DIR "${CMAKE_CURRENT_LIST_DIR}/SDL")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(ESP_LCD_HOST_OS "LINUX")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(ESP_LCD_HOST_OS "MACOS")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows" OR CMAKE_SYSTEM_NAME STREQUAL "MSYS" OR CMAKE_SYSTEM_NAME STREQUAL "CYGWIN")
    set(ESP_LCD_HOST_OS "WINDOWS")
else()
    message(FATAL_ERROR
        "esp_lcd_host does not know the host platform '${CMAKE_SYSTEM_NAME}'. "
        "The component supports Linux, macOS and Windows; the SDL configuration "
        "only has to be told how to build for a new platform.")
endif()

message(STATUS "esp_lcd_host: building SDL for the ${ESP_LCD_HOST_OS} host")

# Only build a static SDL3, and none of the SDL samples, tests or install
# rules. The values are put in the cache so the options of SDL keep them.
set(SDL_SHARED          OFF CACHE BOOL "" FORCE)
set(SDL_STATIC          ON  CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY    OFF CACHE BOOL "" FORCE)
set(SDL_TESTS           OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES        OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL         OFF CACHE BOOL "" FORCE)
set(SDL_DISABLE_INSTALL ON  CACHE BOOL "" FORCE)

# Subsystems the panel does not use. They are on by default, so each one has to
# be named; disabling them also removes their sources and their platform checks
# from the SDL configuration, which is what keeps the build small.
set(SDL_AUDIO    OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA   OFF CACHE BOOL "" FORCE)
set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC   OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI   OFF CACHE BOOL "" FORCE)
set(SDL_POWER    OFF CACHE BOOL "" FORCE)
set(SDL_SENSOR   OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG   OFF CACHE BOOL "" FORCE)
set(SDL_TRAY     OFF CACHE BOOL "" FORCE)
set(SDL_GPU      OFF CACHE BOOL "" FORCE)

# Optional third party libraries SDL would otherwise pick up from whatever is
# installed on the build machine: disabling them keeps the build machine
# independent, and the alternatives SDL falls back to (the iconv() of the C
# library, the generic storage and file system back ends) are the ones this
# component expects. The iconv of the C library stays enabled on purpose: SDL
# defaults it to off on macOS and Windows.
set(SDL_SYSTEM_ICONV ON  CACHE BOOL "" FORCE)
set(SDL_LIBICONV     OFF CACHE BOOL "" FORCE)
set(SDL_FRIBIDI      OFF CACHE BOOL "" FORCE)
set(SDL_LIBTHAI      OFF CACHE BOOL "" FORCE)
set(SDL_DBUS         OFF CACHE BOOL "" FORCE)
set(SDL_IBUS         OFF CACHE BOOL "" FORCE)
set(SDL_LIBURING     OFF CACHE BOOL "" FORCE)
set(SDL_LIBUDEV      OFF CACHE BOOL "" FORCE)

# Video drivers that are tied to a board or to a GPU: off everywhere, which
# also removes their platform checks from the configuration.
set(SDL_RPI      OFF CACHE BOOL "" FORCE)
set(SDL_ROCKCHIP OFF CACHE BOOL "" FORCE)
set(SDL_VIVANTE  OFF CACHE BOOL "" FORCE)

# Render drivers the panel never selects: it renders through the SDL software
# renderer and presents with SDL_RenderPresent(). OpenGL ES is left at its
# default of on on purpose: SDL only probes for EGL - which the KMSDRM driver
# needs - when OpenGL or OpenGL ES is enabled, and turning it off would
# silently drop the KMSDRM driver even when libdrm and gbm are installed.
set(SDL_OPENGL        OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN        OFF CACHE BOOL "" FORCE)
set(SDL_RENDER_GPU    OFF CACHE BOOL "" FORCE)
set(SDL_RENDER_VULKAN OFF CACHE BOOL "" FORCE)

# Do not record dlopen dependencies in .note.dlopen sections: SDL is built into
# an ESP-IDF application, which does not read those sections.
set(SDL_DLOPEN_NOTES OFF CACHE BOOL "" FORCE)

# The desktop video drivers: on by default, so the preview window opens on the
# machine the application runs on, and each one can be turned off for a build
# that must not probe the desktop packages at all. SDL decides from what the
# machine actually has, loads the desktop libraries with dlopen() at run time
# and keeps the dummy and offscreen drivers for a machine without a display,
# which is what lets the CI build without the desktop libraries.
option(ESP_LCD_HOST_SDL_X11     "Build the SDL X11 video driver when X11 is available" ON)
option(ESP_LCD_HOST_SDL_WAYLAND "Build the SDL Wayland video driver when Wayland is available" ON)
option(ESP_LCD_HOST_SDL_KMSDRM  "Build the SDL KMSDRM video driver when libdrm, gbm and EGL are available" ON)
set(SDL_X11     ${ESP_LCD_HOST_SDL_X11}     CACHE BOOL "" FORCE)
set(SDL_WAYLAND ${ESP_LCD_HOST_SDL_WAYLAND} CACHE BOOL "" FORCE)
set(SDL_KMSDRM  ${ESP_LCD_HOST_SDL_KMSDRM}  CACHE BOOL "" FORCE)

# SDL refuses to configure a Unix build that can neither create an X11 nor a
# Wayland window, because such a build cannot open a window on a normal desktop.
# The panel is not a normal desktop application: it draws into a frame buffer
# and only opens a preview window when the machine has a display server. Allow
# the console build when both desktop drivers are turned off (the CI does, so
# that the Linux build only depends on the toolchain): the driver list of such
# a build is then "dummy kmsdrm offscreen".
if(ESP_LCD_HOST_OS STREQUAL "LINUX" AND NOT ESP_LCD_HOST_SDL_X11 AND NOT ESP_LCD_HOST_SDL_WAYLAND)
    set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
endif()

# SDL and the ESP-IDF build system both vendor the same include-guarded
# GetGitRevisionDescription module. The build system includes its copy first,
# so the guard variable is already set here and the include of the SDL copy
# would return early: the git_describe() that stays active is then the one of
# the build system, which takes a second argument that the SDL project does not
# pass, and the SDL configuration fails with "Function invoked with incorrect
# arguments". Clear the guard so the SDL project registers its own copy. The
# function definitions of a subdirectory do not leak back into the build
# system, so this only affects the SDL project itself.
unset(__get_git_revision_description)

# idf_component_register() seeds the directory scope with the include
# directories, the compile options and the definitions of the build system (the
# linux build compiles everything with -Werror and a fixed C standard, which
# the SDL sources do not survive), plus the common link libraries: an exported
# target may not link targets outside the export set. Snapshot all of that onto
# the component target and reset the directory scope, so the SDL subproject
# configures with the flags of its own project, exactly like a standalone build
# of SDL, and the component keeps compiling the way the build system configured
# it.
set(_lcd_host_dir_props INCLUDE_DIRECTORIES COMPILE_OPTIONS COMPILE_DEFINITIONS
                        C_COMPILE_OPTIONS CXX_COMPILE_OPTIONS ASM_COMPILE_OPTIONS)
foreach(_lcd_host_prop IN LISTS _lcd_host_dir_props)
    get_property(_lcd_host_val DIRECTORY PROPERTY ${_lcd_host_prop})
    if(_lcd_host_val)
        if(_lcd_host_prop STREQUAL "INCLUDE_DIRECTORIES")
            target_include_directories(${COMPONENT_LIB} PRIVATE ${_lcd_host_val})
        elseif(_lcd_host_prop STREQUAL "COMPILE_DEFINITIONS")
            # The definitions are stored without their -D prefix and the
            # target_compile_definitions command adds it back.
            target_compile_definitions(${COMPONENT_LIB} PRIVATE ${_lcd_host_val})
        else()
            target_compile_options(${COMPONENT_LIB} PRIVATE ${_lcd_host_val})
        endif()
    endif()
    set_property(DIRECTORY PROPERTY ${_lcd_host_prop} "")
endforeach()
unset(_lcd_host_dir_props)
unset(_lcd_host_val)
set_property(DIRECTORY PROPERTY LINK_LIBRARIES "")

add_subdirectory("${SDL_DIR}" "SDL")

# The component has to supply a few definitions SDL wants from the parts of
# itself it does not build (see src/sdl_port_stubs.c). They are added to the
# SDL library, because the stub includes the private SDL_internal.h header,
# which is only on the include path of SDL3::SDL3.
target_sources(SDL3-static PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/sdl_port_stubs.c")

target_link_libraries(${COMPONENT_LIB} PRIVATE SDL3::SDL3)
