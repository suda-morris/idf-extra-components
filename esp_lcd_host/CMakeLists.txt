# Build the component: an esp_lcd panel driver backed by the SDL3 submodule.
# It only builds for the ESP-IDF host (linux) target.
#
# SDL3 is vendored as the submodule esp_lcd_host/SDL and is built in this file
# with the CMake project of SDL itself: add_subdirectory() reuses the source
# lists and generates SDL_build_config.h, so the SDL submodule is the only thing
# to review when it is updated. It is not taken from the component registry,
# because the component only has to work on the host and this way it can build
# SDL with the subsystems the LCD driver needs, and without the board support
# packages of the registry component.
#
# idf_component_register() must come after the SDL subproject: the build system
# includes this file once in script mode to collect the component requirements,
# and there add_subdirectory() is not available at all, so the file ends at the
# first guard below. The subproject also has to stay before the register call in
# the real build pass: idf_component_register() seeds the directory scope with
# the include directories, compile options and the common link libraries of the
# build system, and a subdirectory added afterwards would inherit all of them -
# the SDL library would link the IDF component targets and its install(EXPORT)
# rules would fail on them.

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    # Build the vendored SDL3 with the CMake project of SDL itself, for the ESP-IDF
    # host target.
    #
    # The ESP-IDF host target is named "linux" in the component manifest on every
    # operating system, but it builds a native program: the same component builds
    # and runs on Linux, macOS and Windows. SDL is chosen as the back end exactly
    # because it opens a window on all three, and this port therefore lets the SDL
    # CMake project pick the sources, the video driver and the thread back end of
    # the host platform instead of hard coding them.
    #
    # The port needs the SDL video and event path: the native window driver of the
    # platform (X11, Wayland, KMSDRM, Cocoa or Win32), the dummy and offscreen
    # drivers (for CI and for a headless frame buffer) and the software renderer.
    # Instead of compiling a hand picked list of sources, this file reuses the CMake
    # project that ships with the SDL submodule through add_subdirectory(), so the
    # source lists and the build configuration header (SDL_build_config.h) are the
    # ones SDL already maintains and are generated automatically.
    #
    # This file therefore only does two things:
    #
    #   1. it turns off what an LCD panel does not need *before* the subdirectory is
    #      added, so SDL skips those sources and their checks, and
    #   2. it answers the small number of checks whose result would otherwise depend
    #      on the machine running CMake.
    #
    # Everything else - the compiler, the C library, the pthread results, inotify,
    # the X11 extensions, Wayland, KMSDRM, EGL, the window driver of the platform -
    # is left to the checks of the SDL project, which is what keeps this port small
    # and in sync with the submodule.
    #
    # The Linux desktop drivers are verified. The macOS and Windows branches only
    # select the window driver of the platform through the SDL project; no runner in
    # this repository builds them, so treat them as unverified until one does.
    #
    # See the README for the exact set of SDL parts that end up in the build.

    set(SDL_DIR "${CMAKE_CURRENT_LIST_DIR}/SDL")

    if(NOT EXISTS "${SDL_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "The SDL submodule is missing at ${SDL_DIR}. "
            "Fetch the submodules of this repository, for example with 'git submodule update --init --recursive'.")
    endif()

    # ---------------------------------------------------------------------------
    # The host platform.
    #
    # The ESP-IDF host target keeps CMAKE_SYSTEM_NAME of the machine it is built on,
    # so the platform is read from there. SDL uses it to select the window driver,
    # the thread back end and the platform sources, and the few checks answered
    # below are selected with it as well.
    # ---------------------------------------------------------------------------

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(ESP_LCD_HOST_OS "LINUX")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(ESP_LCD_HOST_OS "MACOS")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows" OR CMAKE_SYSTEM_NAME STREQUAL "MSYS" OR CMAKE_SYSTEM_NAME STREQUAL "CYGWIN")
        set(ESP_LCD_HOST_OS "WINDOWS")
    else()
        message(FATAL_ERROR
            "esp_lcd_host does not know the host platform '${CMAKE_SYSTEM_NAME}'. "
            "The component supports Linux, macOS and Windows; the SDL port only has to "
            "be told how to build for a new platform.")
    endif()

    message(STATUS "esp_lcd_host: building SDL for the ${ESP_LCD_HOST_OS} host")

    # ---------------------------------------------------------------------------
    # Only build a static SDL3, and none of the SDL samples, tests or install
    # rules. The values are put in the cache so the options of SDL keep them.
    # ---------------------------------------------------------------------------

    set(SDL_SHARED             OFF CACHE BOOL "Build a shared SDL3"        FORCE)
    set(SDL_STATIC             ON  CACHE BOOL "Build a static SDL3"        FORCE)
    set(SDL_TEST_LIBRARY       OFF CACHE BOOL "Build the SDL3_test library" FORCE)
    set(SDL_TESTS              OFF CACHE BOOL "Build the SDL tests"        FORCE)
    set(SDL_EXAMPLES           OFF CACHE BOOL "Build the SDL examples"     FORCE)
    set(SDL_INSTALL            OFF CACHE BOOL "Install SDL3"               FORCE)
    set(SDL_DISABLE_INSTALL    ON  CACHE BOOL "Do not add install rules"   FORCE)

    # ---------------------------------------------------------------------------
    # Subsystems the panel does not use.
    #
    # Turning these off is what keeps the build small: SDL skips the sources, the
    # platform checks and the configuration macros of a disabled subsystem. It is
    # platform independent, so it is done everywhere.
    # ---------------------------------------------------------------------------

    set(SDL_AUDIO    OFF CACHE BOOL "Disable the SDL audio subsystem"    FORCE)
    set(SDL_CAMERA   OFF CACHE BOOL "Disable the SDL camera subsystem"   FORCE)
    set(SDL_JOYSTICK OFF CACHE BOOL "Disable the SDL joystick subsystem" FORCE)
    set(SDL_HAPTIC   OFF CACHE BOOL "Disable the SDL haptic subsystem"   FORCE)
    set(SDL_HIDAPI   OFF CACHE BOOL "Disable the SDL hidapi subsystem"   FORCE)
    set(SDL_POWER    OFF CACHE BOOL "Disable the SDL power subsystem"    FORCE)
    set(SDL_SENSOR   OFF CACHE BOOL "Disable the SDL sensor subsystem"   FORCE)
    set(SDL_DIALOG   OFF CACHE BOOL "Disable the SDL dialog subsystem"   FORCE)
    set(SDL_TRAY     OFF CACHE BOOL "Disable the SDL tray subsystem"     FORCE)
    set(SDL_GPU      OFF CACHE BOOL "Disable the SDL GPU subsystem"      FORCE)

    # The dummy and offscreen drivers back the panel on a machine without a display
    # (this is the path that the CI uses), so they are kept on every platform.
    set(SDL_OFFSCREEN       ON  CACHE BOOL "Keep the offscreen video driver"     FORCE)
    set(SDL_DUMMYVIDEO      ON  CACHE BOOL "Keep the dummy video driver"         FORCE)

    # Video drivers that are tied to a board or to a GPU. They are off everywhere,
    # and turning them off also removes their platform checks from the
    # configuration.
    set(SDL_RPI             OFF CACHE BOOL "Disable the Raspberry Pi video driver" FORCE)
    set(SDL_ROCKCHIP        OFF CACHE BOOL "Disable the Rockchip video driver"     FORCE)
    set(SDL_VIVANTE         OFF CACHE BOOL "Disable the Vivante video driver"      FORCE)
    set(SDL_OPENVR          OFF CACHE BOOL "Disable the OpenVR video driver"       FORCE)

    # Render drivers the panel never selects. It renders through the SDL software
    # renderer, which is always built with the video subsystem; the window itself
    # is painted by SDL_UpdateWindowSurface().
    #
    # OpenGL ES cannot be turned off, because the KMSDRM video driver of SDL is
    # built on top of EGL and SDL only probes for EGL when OpenGL or OpenGL ES is
    # on. Turning it off would silently drop the KMSDRM driver even when libdrm and
    # gbm are installed. The OpenGL ES render driver that comes with it is small,
    # self contained and never selected by the panel.
    set(SDL_OPENGL          OFF CACHE BOOL "Disable the OpenGL render driver"  FORCE)
    set(SDL_OPENGLES        ON  CACHE BOOL "Keep the EGL support KMSDRM needs" FORCE)
    set(SDL_VULKAN          OFF CACHE BOOL "Disable Vulkan support"            FORCE)
    set(SDL_RENDER_GPU      OFF CACHE BOOL "Disable the SDL_GPU render driver" FORCE)
    set(SDL_RENDER_VULKAN   OFF CACHE BOOL "Disable the Vulkan render driver"  FORCE)

    # ---------------------------------------------------------------------------
    # Video drivers that depend on the desktop packages of the build machine.
    #
    # The point of the component is to use the panel as a screen simulator, so the
    # preview window has to open on the machine the build runs on, and both X11 and
    # Wayland are wanted: SDL picks the right one at run time, so the same binary
    # works in a Wayland session (native Wayland), in an X11 session and under
    # XWayland.
    #
    # They are on by default, but they can be turned off for a build that must not
    # depend on what is installed on the build machine at all. Both the option and
    # its <name>_ISSET companion are written, because SDL declares these drivers
    # with cmake_dependent_option() and the macro of CMake only reads a value back
    # through the _ISSET variable. It has to be a cache entry, because cmake only
    # rebuilds the cache entry when it did not find one already: a plain variable
    # is enough for the _ISSET check, but not for the SDL project, which reads the
    # cache entries again.
    #
    # Everything else about the desktop drivers is probed by SDL itself: the
    # packages, the X11 extensions, EGL and the Wayland libraries. The port only
    # raises the option; SDL decides from what the machine actually has.
    # ---------------------------------------------------------------------------

    # The drivers are loaded with dlopen() at run time, which is what a desktop build
    # of SDL does. This is also what keeps them optional: a driver that is compiled in
    # does not stop the executable from running on a machine without the desktop
    # libraries, it just never gets selected.
    set(SDL_DEPS_SHARED ON CACHE BOOL "Load the optional drivers with dlopen()" FORCE)

    option(ESP_LCD_HOST_SDL_X11     "Build the SDL X11 video driver when X11 is available"         ON)
    option(ESP_LCD_HOST_SDL_WAYLAND "Build the SDL Wayland video driver when Wayland is available" ON)
    option(ESP_LCD_HOST_SDL_KMSDRM  "Build the SDL KMSDRM video driver when libdrm, gbm and EGL are available" ON)

    function(esp_lcd_host_sdl_set_option name)
        set(${name} ${ARGN} CACHE BOOL "SDL option set by the esp_lcd_host port" FORCE)
        set(${name}_ISSET ${ARGN} CACHE BOOL "SDL option set by the esp_lcd_host port" FORCE)
    endfunction()

    # The X11 extensions of SDL are all or nothing: when one is on and its headers
    # or its library are missing, SDL stops the configuration with a FATAL_ERROR.
    # They are on here, and SDL probes for them, which is the behaviour of a
    # normal desktop build of SDL: a machine with an incomplete set of X11
    # packages fails the configuration with a message from SDL that names the
    # missing package and the option to turn it off. Set ESP_LCD_HOST_SDL_X11=OFF
    # to build without the X11 driver at all.
    #
    # XInput2 and XFixes have to be turned off together: SDL links XInput2 against
    # XFixes for the PointerBarrier type, and stops the configuration when XFixes
    # is off and XInput2 is on.
    esp_lcd_host_sdl_set_option(SDL_X11_XCURSOR    ${ESP_LCD_HOST_SDL_X11})
    esp_lcd_host_sdl_set_option(SDL_X11_XDBE       ${ESP_LCD_HOST_SDL_X11})
    esp_lcd_host_sdl_set_option(SDL_X11_XINPUT     ${ESP_LCD_HOST_SDL_X11})
    esp_lcd_host_sdl_set_option(SDL_X11_XFIXES     ${ESP_LCD_HOST_SDL_X11})
    esp_lcd_host_sdl_set_option(SDL_X11_XRANDR     ${ESP_LCD_HOST_SDL_X11})
    esp_lcd_host_sdl_set_option(SDL_X11_XSCRNSAVER ${ESP_LCD_HOST_SDL_X11})
    esp_lcd_host_sdl_set_option(SDL_X11_XSHAPE     ${ESP_LCD_HOST_SDL_X11})
    esp_lcd_host_sdl_set_option(SDL_X11_XSYNC      ${ESP_LCD_HOST_SDL_X11})
    esp_lcd_host_sdl_set_option(SDL_X11_XTEST      ${ESP_LCD_HOST_SDL_X11})

    # The driver options themselves are last: SDL reads them back when it defines
    # the extensions above, so they have to be in place by then.
    esp_lcd_host_sdl_set_option(SDL_X11     ${ESP_LCD_HOST_SDL_X11})
    esp_lcd_host_sdl_set_option(SDL_WAYLAND ${ESP_LCD_HOST_SDL_WAYLAND})
    esp_lcd_host_sdl_set_option(SDL_KMSDRM  ${ESP_LCD_HOST_SDL_KMSDRM})

    if(ESP_LCD_HOST_SDL_X11)
        # Load the X11 libraries with dlopen() as well, so the executable does not
        # depend on their soname. SDL only falls back to linking them directly when
        # SDL_DEPS_SHARED is off, but the shared mode has to survive the dependency
        # of the option on SDL_X11, hence the explicit value.
        esp_lcd_host_sdl_set_option(SDL_X11_SHARED ON)
    else()
        set(SDL_X11_SHARED OFF CACHE BOOL "The X11 driver is disabled" FORCE)
    endif()

    if(ESP_LCD_HOST_SDL_WAYLAND)
        esp_lcd_host_sdl_set_option(SDL_WAYLAND_SHARED ON)
    else()
        set(SDL_WAYLAND_SHARED OFF CACHE BOOL "The Wayland driver is disabled" FORCE)
    endif()

    # KMSDRM already defaults to the shared mode through SDL_DEPS_SHARED, and its
    # option is a plain dep_option(), so there is nothing to force here.

    # ---------------------------------------------------------------------------
    # Checks answered here.
    #
    # SDL runs a set of check_c_source_compiles()/check_include_file() tests to find
    # out what the host provides. The ones below are answered instead of probed,
    # because their result depends on the C library or on the compiler version of
    # the machine running CMake rather than on the platform, and a test whose result
    # variable is already in the cache is not run at all.
    #
    # Everything that describes the *platform* is left to SDL: the pthread results,
    # HAVE_LIBC, HAVE_DLOPEN, HAVE_CLOCK_GETTIME, the GCC atomics, the Unix and
    # Windows file system and thread back ends, the window driver and the desktop
    # packages. Those all have a stable result on the platform they describe.
    # ---------------------------------------------------------------------------

    # Do not record dlopen dependencies in .note.dlopen sections: SDL is built into
    # an ESP-IDF application, which does not read those sections.
    set(SDL_DLOPEN_NOTES    OFF CACHE BOOL "Do not record dlopen notes" FORCE)

    # The optional third party libraries SDL would otherwise pick up from whatever
    # is installed on the build machine. Neither the panel nor the software renderer
    # uses any of them, and probing for them would make the build machine dependent:
    # a project built on a desktop with Fribidi would link it, the same project on a
    # clean machine would not.
    #
    # The alternatives SDL falls back to are the ones this port expects: the iconv()
    # of the C library, the generic storage and file system back ends. Setting the
    # freeform ones to the empty string is how the port answers a check that reads
    # the value with #ifdef instead of testing the truth of a boolean.
    set(SDL_SYSTEM_ICONV     ON  CACHE BOOL "Use the iconv() of the C library" FORCE)
    set(SDL_LIBICONV         OFF CACHE BOOL "Do not use a separate libiconv"  FORCE)
    set(SDL_FRIBIDI          OFF CACHE BOOL "Disable Fribidi"                 FORCE)
    set(SDL_LIBTHAI          OFF CACHE BOOL "Disable Thai text shaping"       FORCE)
    set(SDL_DBUS             OFF CACHE BOOL "Disable D-Bus support"           FORCE)
    set(SDL_IBUS             OFF CACHE BOOL "Disable IBus support"            FORCE)
    set(SDL_LIBURING         OFF CACHE BOOL "Disable liburing support"        FORCE)
    set(SDL_LIBUDEV          OFF CACHE BOOL "Disable libudev support"         FORCE)

    set(HAVE_FCITX          "" CACHE INTERNAL "No Fcitx")
    set(HAVE_FRIBIDI        "" CACHE INTERNAL "No Fribidi")
    set(HAVE_FRIBIDI_H      "" CACHE INTERNAL "No Fribidi")
    set(HAVE_LIBTHAI        "" CACHE INTERNAL "No libthai")
    set(HAVE_LIBTHAI_H      "" CACHE INTERNAL "No libthai")
    set(HAVE_DBUS_DBUS_H    "" CACHE INTERNAL "No D-Bus")
    set(HAVE_IBUS_IBUS_H    "" CACHE INTERNAL "No IBus")
    set(HAVE_LIBUSB         "" CACHE INTERNAL "No libusb")

    # SDL declares the console keyboard and camera results only for the platforms
    # that have them, so they are answered where they exist.
    if(ESP_LCD_HOST_OS STREQUAL "LINUX")
        set(HAVE_INPUT_KBIO        "" CACHE INTERNAL "No BSD keyboard ioctls")
        set(HAVE_INPUT_WSCONS      "" CACHE INTERNAL "No wscons keyboard ioctls")
    elseif(ESP_LCD_HOST_OS STREQUAL "MACOS")
        set(HAVE_LINUX_INPUT_H     "" CACHE INTERNAL "No linux/input.h on macOS")
        set(HAVE_LINUX_VIDEODEV2_H "" CACHE INTERNAL "No V4L2 camera on macOS")
        set(HAVE_INPUT_KD          "" CACHE INTERNAL "No console keyboard ioctls on macOS")
    else()
        set(HAVE_LINUX_INPUT_H     "" CACHE INTERNAL "No linux/input.h on Windows")
        set(HAVE_LINUX_VIDEODEV2_H "" CACHE INTERNAL "No V4L2 camera on Windows")
        set(HAVE_INPUT_KD          "" CACHE INTERNAL "No console keyboard ioctls on Windows")
    endif()

    # ---------------------------------------------------------------------------
    # The Unix console check of SDL.
    #
    # SDL refuses to configure a Unix build that can neither create an X11 nor a
    # Wayland window, because such a build cannot open a window on a normal desktop.
    # The panel is not a normal desktop application: it draws into a frame buffer
    # and only opens a preview window when the machine has a display server, so this
    # is not an error here.
    #
    # It matters for the build that turns both drivers off (the CI does, so that the
    # Linux build only depends on the toolchain in the espressif/idf image) and for
    # a machine that has neither set of development packages installed. The check is
    # only about the drivers being compiled in: the panel works without them and the
    # driver list of such a build is "dummy kmsdrm offscreen".
    # ---------------------------------------------------------------------------

    if(ESP_LCD_HOST_OS STREQUAL "LINUX")
        message(STATUS "esp_lcd_host: SDL_X11=${ESP_LCD_HOST_SDL_X11} SDL_WAYLAND=${ESP_LCD_HOST_SDL_WAYLAND} SDL_KMSDRM=${ESP_LCD_HOST_SDL_KMSDRM}")

        if(NOT ESP_LCD_HOST_SDL_X11 AND NOT ESP_LCD_HOST_SDL_WAYLAND)
            set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "Do not require X11 or Wayland" FORCE)
        endif()
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

    # Add the submodule as a subdirectory. Its CMake project generates
    # SDL_build_config.h from include/build_config/SDL_build_config.h.cmake with the
    # options and the check results above, selects the sources and the video driver
    # of the host platform, and defines the SDL3::SDL3 target.
    add_subdirectory("${SDL_DIR}" "SDL")

    # The port has to supply a few definitions SDL wants from the parts of itself it
    # does not build (see src/sdl_port_stubs.c). They are added to the SDL
    # library, because the stub includes the private SDL_internal.h header, which is
    # only on the include path of SDL3::SDL3.
    target_sources(SDL3-static PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/sdl_port_stubs.c")
endif()

idf_component_register(SRCS "src/esp_lcd_host.c"
                            "src/esp_lcd_host_screenshot.c"
                            "src/esp_lcd_host_sdl_backend.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       REQUIRES esp_lcd)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    target_link_libraries(${COMPONENT_LIB} PRIVATE SDL3::SDL3)
endif()
