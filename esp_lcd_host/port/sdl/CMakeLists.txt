# Build the vendored SDL3 with the CMake project of SDL itself, for the
# ESP-IDF host target.
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
# To keep the build reproducible and small, every SDL option the LCD panel does
# not use is turned off *before* the subdirectory is added. Only the checks that
# depend on the build machine *and* on the platform (the Linux desktop packages,
# the pthread and inotify results) are answered here, and only on the platform
# they describe: macOS and Windows rely on the SDL checks for their own
# platform, which have a stable result there.
#
# See the README for the exact set of SDL parts that end up in the build.

set(SDL_DIR "${CMAKE_CURRENT_LIST_DIR}/../../SDL")

if(NOT EXISTS "${SDL_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "The SDL submodule is missing at ${SDL_DIR}. "
        "Fetch the submodules of this repository, for example with 'git submodule update --init --recursive'.")
endif()

# ---------------------------------------------------------------------------
# The host platform.
#
# The ESP-IDF host target keeps CMAKE_SYSTEM_NAME of the machine it is built on,
# so the platform is read from there. It decides which video driver and which
# thread back end of SDL are used, and which of the checks below are answered
# here.
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

# The panel renders through the SDL software renderer, which is always built
# with the video subsystem, so no GPU render driver is needed.
set(SDL_OPENGL          OFF CACHE BOOL "Disable the OpenGL render driver" FORCE)
set(SDL_VULKAN          OFF CACHE BOOL "Disable Vulkan support"          FORCE)
set(SDL_RENDER_GPU      OFF CACHE BOOL "Disable the SDL_GPU render driver" FORCE)
set(SDL_RENDER_VULKAN   OFF CACHE BOOL "Disable the Vulkan render driver"  FORCE)

# Optional dependencies that would otherwise be picked up from whatever is
# installed on the build machine. Neither the panel nor the software renderer
# uses any of them, and probing for them would make the build machine
# dependent. The alternatives SDL falls back to (iconv of the C library, the
# generic storage and filesystem back ends, the native window driver) are the
# ones this port expects.
set(SDL_SYSTEM_ICONV     ON  CACHE BOOL "Use the iconv() of the C library" FORCE)
set(SDL_LIBICONV         OFF CACHE BOOL "Do not use a separate libiconv"  FORCE)
set(SDL_FRIBIDI          OFF CACHE BOOL "Disable Fribidi"                 FORCE)
set(SDL_LIBTHAI          OFF CACHE BOOL "Disable Thai text shaping"       FORCE)
set(SDL_DBUS              OFF CACHE BOOL "Disable D-Bus support"          FORCE)
set(SDL_IBUS              OFF CACHE BOOL "Disable IBus support"           FORCE)
set(SDL_LIBURING          OFF CACHE BOOL "Disable liburing support"       FORCE)
set(SDL_LIBUDEV           OFF CACHE BOOL "Disable libudev support"        FORCE)

# The dynamic API is designed to let an application replace the SDL shared
# library at run time. SDL is linked statically into the application here, so
# there is nothing to replace and the extra indirection is not wanted.
set(SDL_DEPS_SHARED       OFF CACHE BOOL "Do not load any dependency at run time" FORCE)

# ---------------------------------------------------------------------------
# Video drivers that are tied to a board or to a GPU. They are off everywhere,
# and turning them off also removes their platform checks from the
# configuration.
# ---------------------------------------------------------------------------

set(SDL_RPI             OFF CACHE BOOL "Disable the Raspberry Pi video driver"   FORCE)
set(SDL_ROCKCHIP        OFF CACHE BOOL "Disable the Rockchip video driver"   FORCE)
set(SDL_VIVANTE         OFF CACHE BOOL "Disable the Vivante video driver"    FORCE)
set(SDL_OPENVR          OFF CACHE BOOL "Disable the OpenVR video driver"     FORCE)

# ---------------------------------------------------------------------------
# Linux: the desktop and console drivers, and the platform checks of the Linux
# host.
#
# On macOS and Windows the native window driver (Cocoa, Win32) is picked up by
# SDL itself and needs no option, so this whole section is skipped there.
# ---------------------------------------------------------------------------

if(ESP_LCD_HOST_OS STREQUAL "LINUX")

    # SDL refuses to configure a Unix build that can neither create an X11 nor a
    # Wayland window. The panel does not need either: it draws into an offscreen
    # framebuffer and only opens a window when a DRM device is available, or in
    # the X11 or Wayland session below.
    set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "Do not require X11 or Wayland" FORCE)

    # Sets SDL option <name> to <value>, in a way cmake_dependent_option() of SDL
    # picks up. SDL declares the desktop video driver options with
    # cmake_dependent_option(), which does not read a plain cache value back:
    # the entry written below is read as OFF, because "<name>" appears in the
    # dependency list of the macro and is therefore listed as unset for the next
    # option. The macro does honour a value in the <name>_ISSET variable, so
    # every option has to be set through that as well. It has to be a cache
    # entry, otherwise the nested project gets a fresh and empty variable.
    function(esp_lcd_host_sdl_set_option name value)
        set(${name} ${value} CACHE BOOL "SDL option set by the esp_lcd_host port" FORCE)
        set(${name}_ISSET ${value} CACHE BOOL "SDL option set by the esp_lcd_host port" FORCE)
    endfunction()

    # -----------------------------------------------------------------------
    # Desktop window drivers: X11 and Wayland.
    #
    # The point of the component is to use the panel as a screen simulator, so
    # the preview window has to open on the machine the build runs on. That is
    # why both are enabled by default: SDL probes for them, and whichever one
    # the machine has development packages for is compiled in. A Wayland session
    # needs the Wayland driver, an X11 session (or an XWayland one) the X11
    # driver, so both are wanted and SDL picks the right one at run time.
    #
    # Turning them off is possible, for a build that must not depend on what is
    # installed on the build machine (CI passes both OFF):
    #
    #   -DESP_LCD_HOST_SDL_X11=OFF
    #   -DESP_LCD_HOST_SDL_WAYLAND=OFF
    #
    # The offscreen and dummy drivers always remain, so the panel keeps working
    # as a framebuffer on a machine without either.
    # -----------------------------------------------------------------------

    option(ESP_LCD_HOST_SDL_X11 "Build the SDL X11 video driver when X11 is available" ON)
    option(ESP_LCD_HOST_SDL_WAYLAND "Build the SDL Wayland video driver when Wayland is available" ON)

    # Wayland is a plain opt-in: SDL probes for the Wayland packages and links
    # them when the option is on, and there is nothing to pin beforehand. Load
    # the libraries with dlopen() at run time, which is what a desktop build
    # normally does and which keeps the executable independent of the exact
    # soname of those libraries.
    esp_lcd_host_sdl_set_option(SDL_WAYLAND ${ESP_LCD_HOST_SDL_WAYLAND})
    esp_lcd_host_sdl_set_option(SDL_WAYLAND_SHARED ${ESP_LCD_HOST_SDL_WAYLAND})
    esp_lcd_host_sdl_set_option(SDL_WAYLAND_LIBDECOR OFF)
    esp_lcd_host_sdl_set_option(SDL_WAYLAND_LIBDECOR_SHARED OFF)

    # The X11 extensions of the driver.
    #
    # SDL treats each of them as all or nothing: when the option is on and its
    # headers or its library are missing, the configuration stops with a
    # FATAL_ERROR; when the option is off, the extension is left out of the
    # build.
    #
    # The catch is that the X11 driver of SDL is not clean in that second case:
    #
    #   - SDL_x11toolkit.c has a `NOXRANDR:` label that is only reachable when
    #     SDL_VIDEO_DRIVER_X11_XRANDR is set, so with the option off the build
    #     fails with `label defined but not used`,
    #   - SDL_x11window.c keeps the `use_resize_sync` variable when the XSync
    #     option is off, which is an `unused variable`,
    #   - <X11/extensions/XInput2.h> includes <X11/extensions/Xfixes.h> for its
    #     PointerBarrier type, so XFixes is a dependency of XInput, not an
    #     add-on. SDL checks both together (`SDL_X11_XFIXES AND HAVE_XFIXES_H_
    #     AND HAVE_XINPUT2_H`) and stops the configuration with a FATAL_ERROR
    #     when XFixes is on but XInput2 is not, so XInput2 belongs to the same
    #     required set as XFixes and has to be probed with it, not on its own.
    #
    # The cases above are fatal either under the -Werror of the CI (the first
    # two) or inside the SDL configuration (the third), so these are required in
    # practice and the port only turns the X11 driver on when all of them are
    # available. The rest (Xcursor, Xdbe, XScrnSaver, XShape, XTest) are probed
    # for one by one and left off when they are not installed, which keeps a
    # machine with an incomplete set of X11 packages working.
    #
    # The probes use the same packages SDL looks for, so the port never asks for
    # more than SDL itself would.
    # -----------------------------------------------------------------------

    find_package(X11 QUIET)

    # find_package(X11) only looks for the libraries: the headers have to be
    # checked on their own, or a system with the runtime packages but without
    # the -dev ones looks like a system that can build the driver. SDL checks
    # the same two headers before it compiles anything.
    find_path(_esp_lcd_host_x11_xlib_header NAMES X11/Xlib.h)
    find_path(_esp_lcd_host_x11_xext_header NAMES X11/extensions/Xext.h)

    if(NOT X11_FOUND OR NOT _esp_lcd_host_x11_xlib_header OR NOT _esp_lcd_host_x11_xext_header)
        message(STATUS "esp_lcd_host: the X11 development packages are not installed, the X11 video driver is disabled")
        set(_esp_lcd_host_x11_headers FALSE)
    else()
        set(_esp_lcd_host_x11_headers TRUE)
    endif()

    # One entry per line: option, header, library. An empty library means the
    # header is enough.
    # XInput2 is in the required set together with XFixes: SDL fails the
    # configuration when XFixes is on and XInput2 is not (see above), so both
    # have to be available for the driver to be built at all.
    set(_esp_lcd_host_x11_extensions_required
        "SDL_X11_XFIXES X11/extensions/Xfixes.h Xfixes"
        "SDL_X11_XINPUT X11/extensions/XInput2.h Xi"
        "SDL_X11_XRANDR X11/extensions/Xrandr.h Xrandr"
        "SDL_X11_XSYNC X11/extensions/sync.h Xext"
    )
    set(_esp_lcd_host_x11_extensions_optional
        "SDL_X11_XCURSOR X11/Xcursor/Xcursor.h Xcursor"
        "SDL_X11_XDBE X11/extensions/Xdbe.h Xext"
        "SDL_X11_XSCRNSAVER X11/extensions/scrnsaver.h Xss"
        "SDL_X11_XSHAPE X11/extensions/shape.h "
        "SDL_X11_XTEST X11/extensions/XTest.h Xtst"
    )

    set(_esp_lcd_host_x11_required_missing FALSE)
    set(_esp_lcd_host_x11_result)

    foreach(_group required optional)
        foreach(_ext IN LISTS _esp_lcd_host_x11_extensions_${_group})
            separate_arguments(_ext)
            list(GET _ext 0 _ext_option)
            list(GET _ext 1 _ext_header)
            list(GET _ext 2 _ext_library)

            set(_ext_found FALSE)
            if(_esp_lcd_host_x11_headers)
                find_path(${_ext_option}_INCLUDE_DIR NAMES "${_ext_header}")
                if(${_ext_option}_INCLUDE_DIR)
                    if(NOT _ext_library)
                        set(_ext_found TRUE)
                    else()
                        find_library(${_ext_option}_LIBRARY NAMES "${_ext_library}")
                        if(${_ext_option}_LIBRARY)
                            set(_ext_found TRUE)
                        endif()
                    endif()
                endif()
            endif()

            if(_ext_found)
                message(STATUS "esp_lcd_host: X11 ${_ext_option} available")
            elseif(_group STREQUAL "required")
                message(STATUS "esp_lcd_host: X11 ${_ext_option} is missing, the X11 video driver will not be built")
                set(_esp_lcd_host_x11_required_missing TRUE)
            else()
                message(STATUS "esp_lcd_host: X11 ${_ext_option} is missing, it is disabled")
            endif()

            # <option> <found> , applied to SDL once the driver state is known.
            set(_esp_lcd_host_x11_result "${_esp_lcd_host_x11_result}${_ext_option} ${_ext_found}|")
        endforeach()
    endforeach()

    # The X11 driver itself is only built when it is wanted and complete: SDL
    # probes for it with find_package(X11), and the required extensions above
    # are checked here, because SDL cannot be asked to skip them.
    if(NOT ESP_LCD_HOST_SDL_X11 OR _esp_lcd_host_x11_required_missing)
        esp_lcd_host_sdl_set_option(SDL_X11 OFF)
    else()
        esp_lcd_host_sdl_set_option(SDL_X11 ON)
    endif()

    # The optional extensions are on when they were found and the driver is
    # built.
    string(REPLACE "|" ";" _esp_lcd_host_x11_result "${_esp_lcd_host_x11_result}")
    foreach(_entry IN LISTS _esp_lcd_host_x11_result)
        if(NOT _entry)
            continue()
        endif()
        separate_arguments(_entry)
        list(GET _entry 0 _ext_option)
        list(GET _entry 1 _ext_found)

        if(_ext_found AND SDL_X11)
            esp_lcd_host_sdl_set_option(${_ext_option} ON)
        else()
            esp_lcd_host_sdl_set_option(${_ext_option} OFF)
        endif()
    endforeach()

    # Load the X11 library with dlopen() at run time as well, so the executable
    # does not depend on the exact soname. SDL only falls back to linking it
    # directly when SDL_DEPS_SHARED is off, which is not the case here, but then
    # the shared mode has to survive the dependency on SDL_X11 above, hence the
    # same helper.
    esp_lcd_host_sdl_set_option(SDL_X11_SHARED ${SDL_X11})

    # OpenGL ES stays enabled for one reason: SDL only looks for EGL when OpenGL
    # or OpenGL ES is on, and the KMSDRM video driver cannot be built without
    # EGL. Turning it off would silently drop the KMSDRM driver even when libdrm
    # and gbm are installed. The OpenGL ES render driver that comes with it is
    # small and self contained, and the panel never selects it.
    set(SDL_OPENGLES        ON  CACHE BOOL "Keep the EGL support KMSDRM needs" FORCE)

    # Do not record dlopen dependencies in .note.dlopen sections on Linux: SDL
    # is built with the ESP-IDF host target, which does not look at those
    # sections.
    set(SDL_DLOPEN_NOTES      OFF CACHE BOOL "Do not record dlopen notes" FORCE)

    # -----------------------------------------------------------------------
    # Platform checks answered here.
    #
    # SDL runs a set of check_c_source_compiles()/check_include_file() tests to
    # find out what the host provides. Most of them have the same result on
    # every Linux machine, so they are pinned to keep the build independent of
    # the exact C library of the build machine: a test whose result variable is
    # already in the cache is not run at all.
    # -----------------------------------------------------------------------

    # The ESP-IDF host target is built with the C library of the host, and the
    # headers below are required by the POSIX host target.
    set(HAVE_LIBC                   ON  CACHE INTERNAL "Use the C library of the host")

    # The Linux host always has pthreads, and SDL uses the pthread
    # implementation of its threads, mutexes, semaphores and condition
    # variables.
    set(HAVE_PTHREADS               1   CACHE INTERNAL "Host has pthreads")
    set(HAVE_PTHREADS_SEM           1   CACHE INTERNAL "Host has pthread semaphores")
    set(HAVE_PTHREAD_H              1   CACHE INTERNAL "Host has pthread.h")
    set(HAVE_RECURSIVE_MUTEXES      1   CACHE INTERNAL "Host has recursive mutexes")
    set(HAVE_PTHREAD_SETNAME_NP     1   CACHE INTERNAL "Host has pthread_setname_np()")
    set(COMPILER_HAS_SEM_TIMEDWAIT  1   CACHE INTERNAL "Host has sem_timedwait()")

    # The host target is built with gcc or clang, which both implement the GCC
    # atomics and visibility attributes SDL looks for.
    set(HAVE_GCC_ATOMICS            1   CACHE INTERNAL "Compiler has GCC atomics")
    set(HAVE_GCC_FVISIBILITY        1   CACHE INTERNAL "Compiler supports -fvisibility")

    # clock_gettime() is part of the C library on the Linux host, so SDL does
    # not have to link against librt.
    set(HAVE_CLOCK_GETTIME          1   CACHE INTERNAL "Host has clock_gettime() in libc")
    set(FOUND_CLOCK_GETTIME_LIBC    1   CACHE INTERNAL "clock_gettime() is in libc")

    # dlopen() is how SDL loads its optional dependencies.
    set(HAVE_DLOPEN                 1   CACHE INTERNAL "Host has dlopen()")

    # The pthread-based thread, the Unix filesystem and the POSIX fsops, time
    # and timer implementations are the ones the Linux host target uses.
    set(SDL_THREAD_PTHREAD              1 CACHE INTERNAL "Use the pthread thread back end")
    set(SDL_THREAD_PTHREAD_RECURSIVE_MUTEX 1 CACHE INTERNAL "Use recursive pthread mutexes")
    set(HAVE_SDL_THREADS            TRUE CACHE INTERNAL "Threads are available")

    # The Linux host target has the evdev and console keyboard headers SDL
    # probes for. They are used by the keyboard and mouse code of the video
    # path.
    set(HAVE_LINUX_INPUT_H          1   CACHE INTERNAL "Host has linux/input.h")
    set(HAVE_INPUT_KD               1   CACHE INTERNAL "Host has the Linux console keyboard ioctls")

    # The inotify API is used by the storage back end to watch files. It is
    # always available on the host target, so SDL does not have to fall back to
    # polling.
    set(HAVE_SYS_INOTIFY_H          1   CACHE INTERNAL "Host has sys/inotify.h")
    set(HAVE_INOTIFY_INIT           1   CACHE INTERNAL "Host has inotify_init()")
    set(HAVE_INOTIFY_INIT1          1   CACHE INTERNAL "Host has inotify_init1()")
    set(HAVE_INOTIFY                1   CACHE INTERNAL "Host has inotify")

    # Console keyboards, V4L2 and the optional libraries that are turned off
    # above. Answering them here keeps their probe out of the configuration.
    set(HAVE_INPUT_KBIO            "" CACHE INTERNAL "No BSD keyboard ioctls")
    set(HAVE_INPUT_WSCONS          "" CACHE INTERNAL "No wscons keyboard ioctls")
    set(HAVE_LINUX_VIDEODEV2_H     "" CACHE INTERNAL "No V4L2 camera")
    set(HAVE_LIBUDEV_H             "" CACHE INTERNAL "No libudev")
    set(HAVE_LIBURING_H            "" CACHE INTERNAL "No liburing")
    set(HAVE_DBUS_DBUS_H           "" CACHE INTERNAL "No D-Bus")
    set(HAVE_IBUS_IBUS_H           "" CACHE INTERNAL "No IBus")
    set(HAVE_FCITX                 "" CACHE INTERNAL "No Fcitx")
    set(HAVE_FRIBIDI               "" CACHE INTERNAL "No Fribidi")
    set(HAVE_FRIBIDI_H             "" CACHE INTERNAL "No Fribidi")
    set(HAVE_LIBTHAI               "" CACHE INTERNAL "No libthai")
    set(HAVE_LIBTHAI_H             "" CACHE INTERNAL "No libthai")
    set(HAVE_LIBUSB                "" CACHE INTERNAL "No libusb")

    # SDL sets SDL_LOADSO_DLOPEN itself from the HAVE_DLOPEN result pinned
    # above, so the desktop drivers are loaded with dlopen().

elseif(ESP_LCD_HOST_OS STREQUAL "MACOS")

    # macOS brings its own frameworks: the Cocoa video driver, the pthread
    # thread back end and the CoreVideo pieces are all part of the system, so
    # SDL detects them with its Apple checks and there is nothing to pin here.
    # Only the optional third party dependencies that would otherwise be picked
    # up if they happen to be installed through Homebrew or MacPorts are turned
    # off, which is what keeps the build reproducible.

    # The optional libraries SDL would probe for. The panel does not use any of
    # them, and on macOS they are only present when the user installed them, so
    # probing would make the build machine dependent.
    set(HAVE_FRIBIDI               "" CACHE INTERNAL "No Fribidi")
    set(HAVE_FRIBIDI_H             "" CACHE INTERNAL "No Fribidi")
    set(HAVE_LIBTHAI               "" CACHE INTERNAL "No libthai")
    set(HAVE_LIBTHAI_H             "" CACHE INTERNAL "No libthai")
    set(HAVE_DBUS_DBUS_H           "" CACHE INTERNAL "No D-Bus")
    set(HAVE_IBUS_IBUS_H           "" CACHE INTERNAL "No IBus")
    set(HAVE_LIBUSB                "" CACHE INTERNAL "No libusb")

else() # ESP_LCD_HOST_OS STREQUAL "WINDOWS"

    # Windows brings its own window driver (Win32) and its own thread, timing
    # and file system back ends, all detected by the SDL Windows checks, so
    # there is nothing to pin here either. The optional libraries are turned off
    # for the same reason as on macOS: they would only be picked up when the
    # build machine happens to have them.
    set(HAVE_FRIBIDI               "" CACHE INTERNAL "No Fribidi")
    set(HAVE_FRIBIDI_H             "" CACHE INTERNAL "No Fribidi")
    set(HAVE_LIBTHAI               "" CACHE INTERNAL "No libthai")
    set(HAVE_LIBTHAI_H             "" CACHE INTERNAL "No libthai")
    set(HAVE_DBUS_DBUS_H           "" CACHE INTERNAL "No D-Bus")
    set(HAVE_IBUS_IBUS_H           "" CACHE INTERNAL "No IBus")
    set(HAVE_LIBUSB                "" CACHE INTERNAL "No libusb")

endif()

# Add the submodule as a subdirectory. Its CMake project generates
# SDL_build_config.h from include/build_config/SDL_build_config.h.cmake with the
# options and the check results above, selects the sources and the video driver
# of the host platform, and defines the SDL3::SDL3 target.
add_subdirectory("${SDL_DIR}" "SDL")

# The port has to supply a few definitions SDL wants from the parts of itself it
# does not build (see port/src/sdl_port_stubs.c). They are added to the SDL
# library, because the stub includes the private SDL_internal.h header, which is
# only on the include path of SDL3::SDL3.
target_sources(SDL3-static PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src/sdl_port_stubs.c")
