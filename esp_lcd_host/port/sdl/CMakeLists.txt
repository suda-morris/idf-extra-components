# Build the SDL3 submodule as a static library for the ESP-IDF host target.
#
# The port only needs the SDL video and event path: the KMSDRM driver (for a
# real framebuffer), the dummy and offscreen drivers (for CI and for a headless
# frame buffer) and the software renderer. Audio, input, camera and the other
# subsystems are disabled in port/include/sdl_build_config/SDL_build_config.h.
#
# SDL3 has no option to build a subset of itself through its own CMake project,
# and running that project inside an ESP-IDF build is not supported, so this
# port compiles the sources it needs directly.
#
# The port is added to the build as a CMake subdirectory of the component, so
# that SDL is built by the same build system as the component.
#
# port/src/sdl_port_stubs.c holds the few definitions SDL expects from the parts
# of itself this port does not compile.

set(SDL_DIR "${CMAKE_CURRENT_LIST_DIR}/../../SDL")
string(TOUPPER "${CMAKE_SYSTEM_NAME}" SDL_PORT_SYSTEM_NAME_UPPER)

if(NOT EXISTS "${SDL_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "The SDL submodule is missing at ${SDL_DIR}. "
        "Fetch the submodules of this repository, for example with 'git submodule update --init --recursive'.")
endif()

if(NOT SDL_PORT_SYSTEM_NAME_UPPER MATCHES "LINUX")
    message(FATAL_ERROR
        "esp_lcd_host only supports the ESP-IDF host target on Linux. "
        "The current host system is '${CMAKE_SYSTEM_NAME}'.")
endif()

# ---------------------------------------------------------------------------
# SDL sources
#
# The list below mirrors the sources the CMakeLists.txt of the submodule
# compiles for a Linux system with SDL_AUDIO, SDL_CAMERA, SDL_JOYSTICK,
# SDL_HAPTIC and SDL_HIDAPI off, and the video drivers SDL_build_config.h
# enables. Keep it in sync when the submodule is updated.
# ---------------------------------------------------------------------------

set(SDL_PORT_SRCS
    # shared, always compiled
    "${SDL_DIR}/src/SDL.c"
    "${SDL_DIR}/src/SDL_assert.c"
    "${SDL_DIR}/src/SDL_error.c"
    "${SDL_DIR}/src/SDL_guid.c"
    "${SDL_DIR}/src/SDL_hashtable.c"
    "${SDL_DIR}/src/SDL_hints.c"
    "${SDL_DIR}/src/SDL_list.c"
    "${SDL_DIR}/src/SDL_log.c"
    "${SDL_DIR}/src/SDL_properties.c"
    "${SDL_DIR}/src/SDL_utils.c"
    "${SDL_DIR}/src/atomic/SDL_atomic.c"
    "${SDL_DIR}/src/atomic/SDL_spinlock.c"
    "${SDL_DIR}/src/core/SDL_core_unsupported.c"
    "${SDL_DIR}/src/cpuinfo/SDL_cpuinfo.c"
    "${SDL_DIR}/src/dynapi/SDL_dynapi.c"
    "${SDL_DIR}/src/events/SDL_categories.c"
    "${SDL_DIR}/src/events/SDL_clipboardevents.c"
    "${SDL_DIR}/src/events/SDL_displayevents.c"
    "${SDL_DIR}/src/events/SDL_dropevents.c"
    "${SDL_DIR}/src/events/SDL_events.c"
    "${SDL_DIR}/src/events/SDL_eventwatch.c"
    "${SDL_DIR}/src/events/SDL_keyboard.c"
    "${SDL_DIR}/src/events/SDL_keymap.c"
    "${SDL_DIR}/src/events/SDL_keysym_to_keycode.c"
    "${SDL_DIR}/src/events/SDL_keysym_to_scancode.c"
    "${SDL_DIR}/src/events/SDL_mouse.c"
    "${SDL_DIR}/src/events/SDL_pen.c"
    "${SDL_DIR}/src/events/SDL_quit.c"
    "${SDL_DIR}/src/events/SDL_scancode_tables.c"
    "${SDL_DIR}/src/events/SDL_touch.c"
    "${SDL_DIR}/src/events/SDL_windowevents.c"
    "${SDL_DIR}/src/events/imKStoUCS.c"
    "${SDL_DIR}/src/filesystem/SDL_filesystem.c"
    "${SDL_DIR}/src/filesystem/posix/SDL_sysfsops.c"
    "${SDL_DIR}/src/filesystem/unix/SDL_sysfilesystem.c"
    "${SDL_DIR}/src/io/SDL_asyncio.c"
    "${SDL_DIR}/src/io/SDL_iostream.c"
    "${SDL_DIR}/src/io/generic/SDL_asyncio_generic.c"
    "${SDL_DIR}/src/locale/SDL_locale.c"
    "${SDL_DIR}/src/locale/unix/SDL_syslocale.c"
    "${SDL_DIR}/src/main/SDL_main_callbacks.c"
    "${SDL_DIR}/src/main/SDL_runapp.c"
    "${SDL_DIR}/src/main/generic/SDL_sysmain_callbacks.c"
    "${SDL_DIR}/src/misc/SDL_url.c"
    "${SDL_DIR}/src/misc/unix/SDL_sysurl.c"
    "${SDL_DIR}/src/process/SDL_process.c"
    "${SDL_DIR}/src/process/posix/SDL_posixprocess.c"
    "${SDL_DIR}/src/render/SDL_render.c"
    "${SDL_DIR}/src/render/SDL_render_unsupported.c"
    "${SDL_DIR}/src/render/SDL_yuv_sw.c"
    "${SDL_DIR}/src/render/software/SDL_blendfillrect.c"
    "${SDL_DIR}/src/render/software/SDL_blendline.c"
    "${SDL_DIR}/src/render/software/SDL_blendpoint.c"
    "${SDL_DIR}/src/render/software/SDL_drawline.c"
    "${SDL_DIR}/src/render/software/SDL_drawpoint.c"
    "${SDL_DIR}/src/render/software/SDL_render_sw.c"
    "${SDL_DIR}/src/render/software/SDL_triangle.c"
    "${SDL_DIR}/src/stdlib/SDL_crc16.c"
    "${SDL_DIR}/src/stdlib/SDL_crc32.c"
    "${SDL_DIR}/src/stdlib/SDL_getenv.c"
    "${SDL_DIR}/src/stdlib/SDL_iconv.c"
    "${SDL_DIR}/src/stdlib/SDL_malloc.c"
    "${SDL_DIR}/src/stdlib/SDL_memcpy.c"
    "${SDL_DIR}/src/stdlib/SDL_memmove.c"
    "${SDL_DIR}/src/stdlib/SDL_memset.c"
    "${SDL_DIR}/src/stdlib/SDL_murmur3.c"
    "${SDL_DIR}/src/stdlib/SDL_qsort.c"
    "${SDL_DIR}/src/stdlib/SDL_random.c"
    "${SDL_DIR}/src/stdlib/SDL_stdlib.c"
    "${SDL_DIR}/src/stdlib/SDL_string.c"
    "${SDL_DIR}/src/stdlib/SDL_strtokr.c"
    "${SDL_DIR}/src/storage/SDL_storage.c"
    "${SDL_DIR}/src/storage/generic/SDL_genericstorage.c"
    "${SDL_DIR}/src/thread/SDL_thread.c"
    "${SDL_DIR}/src/thread/pthread/SDL_syscond.c"
    "${SDL_DIR}/src/thread/pthread/SDL_sysmutex.c"
    "${SDL_DIR}/src/thread/pthread/SDL_sysrwlock.c"
    "${SDL_DIR}/src/thread/pthread/SDL_syssem.c"
    "${SDL_DIR}/src/thread/pthread/SDL_systhread.c"
    "${SDL_DIR}/src/thread/pthread/SDL_systls.c"
    "${SDL_DIR}/src/time/SDL_time.c"
    "${SDL_DIR}/src/time/unix/SDL_systime.c"
    "${SDL_DIR}/src/timer/SDL_timer.c"
    "${SDL_DIR}/src/timer/unix/SDL_systimer.c"
    "${SDL_DIR}/src/tray/SDL_tray_utils.c"
    "${SDL_DIR}/src/tray/dummy/SDL_tray.c"
    "${SDL_DIR}/src/video/SDL_blit.c"
    "${SDL_DIR}/src/video/SDL_blit_0.c"
    "${SDL_DIR}/src/video/SDL_blit_1.c"
    "${SDL_DIR}/src/video/SDL_blit_A.c"
    "${SDL_DIR}/src/video/SDL_blit_N.c"
    "${SDL_DIR}/src/video/SDL_blit_auto.c"
    "${SDL_DIR}/src/video/SDL_blit_copy.c"
    "${SDL_DIR}/src/video/SDL_blit_slow.c"
    "${SDL_DIR}/src/video/SDL_bmp.c"
    "${SDL_DIR}/src/video/SDL_clipboard.c"
    "${SDL_DIR}/src/video/SDL_egl.c"
    "${SDL_DIR}/src/video/SDL_fillrect.c"
    "${SDL_DIR}/src/video/SDL_pixels.c"
    "${SDL_DIR}/src/video/SDL_rect.c"
    "${SDL_DIR}/src/video/SDL_RLEaccel.c"
    "${SDL_DIR}/src/video/SDL_rotate.c"
    "${SDL_DIR}/src/video/SDL_stb.c"
    "${SDL_DIR}/src/video/SDL_stretch.c"
    "${SDL_DIR}/src/video/SDL_surface.c"
    "${SDL_DIR}/src/video/SDL_video.c"
    "${SDL_DIR}/src/video/SDL_video_unsupported.c"
    "${SDL_DIR}/src/video/SDL_vulkan_utils.c"
    "${SDL_DIR}/src/video/SDL_yuv.c"
    "${SDL_DIR}/src/video/yuv2rgb/yuv_rgb_std.c"
    # the POSIX support SDL expects on Linux
    "${SDL_DIR}/src/core/unix/SDL_appid.c"
    "${SDL_DIR}/src/core/unix/SDL_fribidi.c"
    "${SDL_DIR}/src/core/unix/SDL_gtk.c"
    "${SDL_DIR}/src/core/unix/SDL_libthai.c"
    "${SDL_DIR}/src/core/unix/SDL_poll.c"
    # always compiled for Linux by the submodule
    "${SDL_DIR}/src/core/linux/SDL_evdev_capabilities.c"
    "${SDL_DIR}/src/core/linux/SDL_threadprio.c"
    "${SDL_DIR}/src/loadso/dlopen/SDL_sysloadso.c"
    "${SDL_DIR}/src/dialog/SDL_dialog.c"
    "${SDL_DIR}/src/dialog/SDL_dialog_utils.c"
    "${SDL_DIR}/src/dialog/dummy/SDL_dummydialog.c"
    # video drivers enabled by SDL_build_config.h
    "${SDL_DIR}/src/video/dummy/SDL_nullevents.c"
    "${SDL_DIR}/src/video/dummy/SDL_nullframebuffer.c"
    "${SDL_DIR}/src/video/dummy/SDL_nullvideo.c"
    "${SDL_DIR}/src/video/offscreen/SDL_offscreenevents.c"
    "${SDL_DIR}/src/video/offscreen/SDL_offscreenframebuffer.c"
    "${SDL_DIR}/src/video/offscreen/SDL_offscreenopengles.c"
    "${SDL_DIR}/src/video/offscreen/SDL_offscreenvideo.c"
    "${SDL_DIR}/src/video/offscreen/SDL_offscreenvulkan.c"
    "${SDL_DIR}/src/video/offscreen/SDL_offscreenwindow.c"
    # definitions of the parts of SDL this port does not compile
    "${CMAKE_CURRENT_LIST_DIR}/../src/sdl_port_stubs.c"
    # headers, so IDEs and the build systems that track them see the sources
    "${SDL_DIR}/src/stdlib/SDL_casefolding.h"
    "${SDL_DIR}/src/stdlib/SDL_getenv_c.h"
    "${SDL_DIR}/src/stdlib/SDL_sysstdlib.h"
    "${SDL_DIR}/src/stdlib/SDL_vacopy.h"
    "${SDL_DIR}/src/io/SDL_asyncio_c.h"
    "${SDL_DIR}/src/io/SDL_iostream_c.h"
    "${SDL_DIR}/src/io/SDL_sysasyncio.h"
    "${SDL_DIR}/src/misc/SDL_libusb.h"
    "${SDL_DIR}/src/misc/SDL_sysurl.h"
)

# ---------------------------------------------------------------------------
# libdrm: the KMSDRM video driver, used when the machine running the
# application has a DRM device (a local console). The dummy and offscreen
# drivers are always available, so the panel keeps working on a headless
# machine when libdrm is not installed.
# ---------------------------------------------------------------------------

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(KMSDRM IMPORTED_TARGET libdrm)
endif()

if(TARGET PkgConfig::KMSDRM)
    list(APPEND SDL_PORT_SRCS
        "${SDL_DIR}/src/video/kmsdrm/SDL_kmsdrmdyn.c"
        "${SDL_DIR}/src/video/kmsdrm/SDL_kmsdrmevents.c"
        "${SDL_DIR}/src/video/kmsdrm/SDL_kmsdrmmouse.c"
        "${SDL_DIR}/src/video/kmsdrm/SDL_kmsdrmopengles.c"
        "${SDL_DIR}/src/video/kmsdrm/SDL_kmsdrmvideo.c"
        "${SDL_DIR}/src/video/kmsdrm/SDL_kmsdrmvulkan.c"
    )
    set(SDL_PORT_HAVE_KMSDRM 1)
endif()

# ---------------------------------------------------------------------------
# The library
# ---------------------------------------------------------------------------

add_library(sdl3_port STATIC)
target_sources(sdl3_port PRIVATE ${SDL_PORT_SRCS})

add_library(SDL3::SDL3 ALIAS sdl3_port)
add_library(SDL3::Headers ALIAS sdl3_port)

# The generated headers of the port come first, then the SDL headers.
target_include_directories(sdl3_port PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/../include/sdl_build_config"
    "${SDL_DIR}/include"
)
# The private headers of SDL, plus the khronos headers that SDL_egl.h includes.
# The public CMake of SDL uses -idirafter for the khronos directory, which is
# required because the SDL headers and the system GL headers use the same names.
target_include_directories(sdl3_port PRIVATE "${SDL_DIR}/src")
target_compile_options(sdl3_port PRIVATE "-idirafter${SDL_DIR}/src/video/khronos")

# 'USING_GENERATED_CONFIG_H' tells SDL to take the build configuration from the
# SDL_build_config.h of the port folder instead of a header of the same name in
# the include directory of the submodule.
# 'SDL_STATIC_LIB' keeps the SDL_main callbacks and the export declarations out
# of the library, because the application links SDL statically.
target_compile_definitions(sdl3_port PRIVATE
    USING_GENERATED_CONFIG_H
    SDL_STATIC_LIB
    "_GNU_SOURCE=1"
)

# The ESP-IDF host target links the executable with -static by default, which
# is how SDL is normally linked inside an application.
target_link_libraries(sdl3_port PUBLIC
    ${CMAKE_DL_LIBS}
    m
    pthread
)

if(TARGET PkgConfig::KMSDRM)
    target_link_libraries(sdl3_port PRIVATE PkgConfig::KMSDRM)
    target_compile_options(sdl3_port PRIVATE "-DLIBDRM_AVAILABLE")
endif()

if(SDL_PORT_HAVE_KMSDRM)
    message(STATUS "esp_lcd_host: SDL built with the KMSDRM video driver (libdrm found)")
else()
    message(STATUS "esp_lcd_host: libdrm not found, SDL built with the dummy and offscreen video drivers only")
endif()
