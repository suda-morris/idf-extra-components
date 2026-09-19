# Build the vendored SDL3 with the CMake project of SDL itself, for the
# ESP-IDF host target.
#
# The port needs the SDL video and event path: the KMSDRM driver (for a real
# framebuffer), the dummy and offscreen drivers (for CI and for a headless
# frame buffer) and the software renderer. Instead of compiling a hand picked
# list of sources, this file reuses the CMake project that ships with the SDL
# submodule through add_subdirectory(), so the source lists and the build
# configuration header (SDL_build_config.h) are the ones SDL already maintains
# and are generated automatically.
#
# To keep the build reproducible and small, every SDL option the LCD panel
# does not use is turned off *before* the subdirectory is added, and the
# platform checks whose result only depends on the machine running CMake are
# answered here instead of being probed.
#
# See the README for the exact set of SDL parts that end up in the build.

set(SDL_DIR "${CMAKE_CURRENT_LIST_DIR}/../../SDL")

if(NOT EXISTS "${SDL_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "The SDL submodule is missing at ${SDL_DIR}. "
        "Fetch the submodules of this repository, for example with 'git submodule update --init --recursive'.")
endif()

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

# SDL refuses to configure a Unix build that can neither create an X11 nor a
# Wayland window. The panel does not need either: it draws into an offscreen
# framebuffer and only opens a window when a DRM device is available.
set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "Do not require X11 or Wayland" FORCE)

# ---------------------------------------------------------------------------
# Subsystems the panel does not use.
#
# Turning these off is what keeps the build small: SDL skips the sources, the
# platform checks and the configuration macros of a disabled subsystem.
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

# ---------------------------------------------------------------------------
# Video drivers the panel does not use.
#
# The video subsystem itself stays on (the panel needs a window surface), but
# the only drivers that are kept are the offscreen and dummy ones, plus KMSDRM
# when libdrm and EGL are present. Everything that needs X11, Wayland or a
# GPU is turned off, which also removes its platform checks from the
# configuration, so the build does not depend on the development packages
# installed on the machine running CMake.
# ---------------------------------------------------------------------------

set(SDL_X11             OFF CACHE BOOL "Disable the X11 video driver"        FORCE)
set(SDL_X11_SHARED      OFF CACHE BOOL "Disable the dynamically loaded X11"  FORCE)
set(SDL_WAYLAND         OFF CACHE BOOL "Disable the Wayland video driver"    FORCE)
set(SDL_WAYLAND_SHARED  OFF CACHE BOOL "Disable the dynamically loaded Wayland" FORCE)
set(SDL_RPI             OFF CACHE BOOL "Disable the Raspberry Pi video driver"   FORCE)
set(SDL_ROCKCHIP        OFF CACHE BOOL "Disable the Rockchip video driver"   FORCE)
set(SDL_VIVANTE         OFF CACHE BOOL "Disable the Vivante video driver"    FORCE)
set(SDL_OPENVR          OFF CACHE BOOL "Disable the OpenVR video driver"     FORCE)
set(SDL_OFFSCREEN       ON  CACHE BOOL "Keep the offscreen video driver"     FORCE)
set(SDL_DUMMYVIDEO      ON  CACHE BOOL "Keep the dummy video driver"         FORCE)

# The panel renders through the SDL software renderer, which is always built
# with the video subsystem, so no GPU render driver is needed.
#
# OpenGL ES stays enabled for one reason: SDL only looks for EGL when OpenGL or
# OpenGL ES is on, and the KMSDRM video driver cannot be built without EGL.
# Turning it off would silently drop the KMSDRM driver even when libdrm and
# gbm are installed. The OpenGL ES render driver that comes with it is small
# and self contained, and the panel never selects it.
set(SDL_OPENGL          OFF CACHE BOOL "Disable the OpenGL render driver" FORCE)
set(SDL_OPENGLES        ON  CACHE BOOL "Keep the EGL support KMSDRM needs" FORCE)
set(SDL_VULKAN          OFF CACHE BOOL "Disable Vulkan support"          FORCE)
set(SDL_RENDER_GPU      OFF CACHE BOOL "Disable the SDL_GPU render driver" FORCE)
set(SDL_RENDER_VULKAN   OFF CACHE BOOL "Disable the Vulkan render driver"  FORCE)

# Optional dependencies that would otherwise be picked up from whatever is
# installed on the build machine. Neither the panel nor the software renderer
# uses any of them, and probing for them would make the build machine
# dependent. The alternatives SDL falls back to (iconv of the C library, the
# generic storage and filesystem back ends, dlopen) are the ones this port
# expects.
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
set(SDL_LOADSO_DLOPEN     ON  CACHE BOOL "Use dlopen() to load shared objects"   FORCE)

# Do not record dlopen dependencies in .note.dlopen sections: SDL is built
# with the ESP-IDF host target, which does not look at those sections.
set(SDL_DLOPEN_NOTES      OFF CACHE BOOL "Do not record dlopen notes" FORCE)

# ---------------------------------------------------------------------------
# Platform checks answered here.
#
# SDL runs a set of check_c_source_compiles()/check_include_file() tests to
# find out what the host provides. Most of them do not apply to the ESP-IDF
# host target, and the ones that do would make the build depend on the exact
# C library of the build machine. The results are pinned here by pre-seeding
# the cache entries the tests use: a test whose result variable is already in
# the cache is not run at all.
# ---------------------------------------------------------------------------

# The ESP-IDF host target is built with the C library of the host, and the
# headers below are required by the POSIX host target.
set(HAVE_LIBC                   ON  CACHE INTERNAL "Use the C library of the host")

# The host target always has pthreads, and SDL uses the pthread implementation
# of its threads, mutexes, semaphores and condition variables.
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

# clock_gettime() is part of the C library on every supported host, so SDL
# does not have to link against librt.
set(HAVE_CLOCK_GETTIME          1   CACHE INTERNAL "Host has clock_gettime() in libc")
set(FOUND_CLOCK_GETTIME_LIBC    1   CACHE INTERNAL "clock_gettime() is in libc")

# dlopen() is how SDL loads its optional dependencies.
set(HAVE_DLOPEN                 1   CACHE INTERNAL "Host has dlopen()")

# The pthread-based thread, the Unix filesystem and the POSIX fsops, time and
# timer implementations are the ones the host target uses.
set(SDL_THREAD_PTHREAD              1 CACHE INTERNAL "Use the pthread thread back end")
set(SDL_THREAD_PTHREAD_RECURSIVE_MUTEX 1 CACHE INTERNAL "Use recursive pthread mutexes")
set(HAVE_SDL_THREADS            TRUE CACHE INTERNAL "Threads are available")

# The Linux host target has the evdev and console keyboard headers SDL probes
# for. They are used by the keyboard and mouse code of the video path.
set(HAVE_LINUX_INPUT_H          1   CACHE INTERNAL "Host has linux/input.h")
set(HAVE_INPUT_KD               1   CACHE INTERNAL "Host has the Linux console keyboard ioctls")

# The inotify API is used by the storage back end to watch files. It is always
# available on the host target, so SDL does not have to fall back to polling.
set(HAVE_SYS_INOTIFY_H          1   CACHE INTERNAL "Host has sys/inotify.h")
set(HAVE_INOTIFY_INIT           1   CACHE INTERNAL "Host has inotify_init()")
set(HAVE_INOTIFY_INIT1          1   CACHE INTERNAL "Host has inotify_init1()")
set(HAVE_INOTIFY                1   CACHE INTERNAL "Host has inotify")

# The console/keyboard handling and the hidden clock_gettime() fallbacks are
# not needed on the host target.
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

# Add the submodule as a subdirectory. Its CMake project generates
# SDL_build_config.h from include/build_config/SDL_build_config.h.cmake with
# the options and the check results above, and defines the SDL3::SDL3 target.
add_subdirectory("${SDL_DIR}" "SDL")

# The port has to supply a few definitions SDL wants from the parts of itself
# it does not build (see port/src/sdl_port_stubs.c). They are added to the SDL
# library, because the stub includes the private SDL_internal.h header, which
# is only on the include path of SDL3::SDL3.
target_sources(SDL3-static PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src/sdl_port_stubs.c")
