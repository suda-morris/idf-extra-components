idf_component_register(
    SRCS "src/esp_raylib.c" "src/esp_raylib_memory.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    PRIV_REQUIRES esp_lcd esp_timer vfs
    )

set(RAYLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/raylib")

# Shadow rcore.c's platform hook: its quoted include
# "platforms/rcore_memory.c" resolves against the includer's own directory
# first, so the copy picks up the port placed next to it -- and takes
# PLATFORM_MEMORY, silencing the no-platform #pragma message.
set(_rcore_shadow "${CMAKE_CURRENT_BINARY_DIR}/rcore_shadow")
configure_file("${RAYLIB_DIR}/src/rcore.c" "${_rcore_shadow}/rcore.c" COPYONLY)
# Our port, at the exact relative path the copy's hook looks for.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/rcore_platform_esp_idf.c"
               "${_rcore_shadow}/platforms/rcore_memory.c" COPYONLY)
# Compile the copy by path, never the submodule's.
target_sources(${COMPONENT_LIB} PRIVATE "${_rcore_shadow}/rcore.c")

set(PLATFORM          "Memory")   # any other value searches for a GPU
set(OPENGL_VERSION    "Software") # unset, GRAPHICS falls back to OpenGL 3.3
set(BUILD_EXAMPLES    OFF)        # add_executable()s with main() and host threads
set(BUILD_SHARED_LIBS OFF)        # nothing on this target loads a .so

# Upstream's install() rules would write into the IDF prefix, and
# install(EXPORT) validates a link interface that cannot resolve here;
set(_raylib_skip_install "${CMAKE_SKIP_INSTALL_RULES}")
set(CMAKE_SKIP_INSTALL_RULES ON)
add_subdirectory("${RAYLIB_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/raylib")
set(CMAKE_SKIP_INSTALL_RULES "${_raylib_skip_install}")
unset(_raylib_skip_install)

# rcore.c is compiled with the ESP-IDF port in this component instead.
# raudio.c/rmodels.c queue because CUSTOMIZE_BUILD=OFF pins the matching
# SUPPORT_MODULE_* variables to config.h's defaults, whatever this file sets.
get_target_property(_raylib_sources raylib SOURCES)
list(FILTER _raylib_sources EXCLUDE REGEX "(rcore|raudio|rmodels)\\.c$")
set_target_properties(raylib PROPERTIES SOURCES "${_raylib_sources}")

target_link_libraries(${COMPONENT_LIB} PUBLIC raylib)

set(_raylib_defs
    # Takes rcore.c's PLATFORM_MEMORY branch into the shadowed port. Upstream
    # injects the same define PUBLIC; pinned here in case that ever stops.
    PLATFORM_MEMORY

    GRAPHICS_API_OPENGL_SOFTWARE

    # Two buffer slots so the platform layer hands a finished frame to the
    # panel while the renderer draws into the other one. The framebuffer pool
    # holds render textures only, so this is two BeginTextureMode() targets.
    RLSW_DOUBLE_BUFFERING=1
    RLSW_MAX_FRAMEBUFFERS=2
    RLSW_MAX_TEXTURES=32

    # No audio and no 3D model loader.
    SUPPORT_MODULE_RMODELS=0
    SUPPORT_MODULE_RAUDIO=0

    # No serial console in a frame loop; no capture, automation, gestures or
    # clipboard hardware to drive.
    SUPPORT_TRACELOG=0
    SUPPORT_CAMERA_SYSTEM=0
    SUPPORT_GESTURES_SYSTEM=0
    SUPPORT_MOUSE_GESTURES=0
    SUPPORT_SCREEN_CAPTURE=0
    SUPPORT_COMPRESSION_API=0
    SUPPORT_AUTOMATION_EVENTS=0
    SUPPORT_CLIPBOARD_IMAGE=0

    # Nothing on this target exports or procedurally generates images: keep
    # stb_image_write and the GenImage* family out of the binary.
    SUPPORT_IMAGE_EXPORT=0
    SUPPORT_IMAGE_GENERATION=0

    # MCU paths and one screen, not a desktop window.
    MAX_FILEPATH_CAPACITY=256
    MAX_FILEPATH_LENGTH=128
    MAX_KEY_PRESSED_QUEUE=4
    MAX_CHAR_PRESSED_QUEUE=4
    MAX_TOUCH_POINTS=1
    MAX_GAMEPADS=1
    MAX_TEXT_BUFFER_LENGTH=512
    MAX_TEXTSPLIT_COUNT=32

    # One draw-call list that fits in internal RAM, half the texture units.
    RL_DEFAULT_BATCH_DRAWCALLS=128
    RL_DEFAULT_BATCH_MAX_TEXTURE_UNITS=2

    RLSW_FRAMEBUFFER_COLOR_TYPE=${CONFIG_RAYLIB_FRAMEBUFFER_COLOR_TYPE}

    # Only 3D drawing through BeginMode3D()/rlgl touches the depth buffer,
    # and this component is 2D-first: pin the smallest so the per-buffer
    # allocation costs 75 KiB at 320x240 instead of up to 300. Reintroduce
    # a Kconfig choice if 3D becomes a target.
    RLSW_FRAMEBUFFER_DEPTH_TYPE=D8
)

foreach(_fmt QOI PNG BMP GIF DDS TTF FNT)
    if(CONFIG_RAYLIB_FILEFORMAT_${_fmt})
        list(APPEND _raylib_defs SUPPORT_FILEFORMAT_${_fmt}=1)
    else()
        list(APPEND _raylib_defs SUPPORT_FILEFORMAT_${_fmt}=0)
    endif()
endforeach()

foreach(_target raylib ${COMPONENT_LIB})
    target_compile_definitions(${_target} PRIVATE ${_raylib_defs})
    target_compile_options(${_target} PRIVATE
        # Renames raylib's RL_* allocators to ours and declares them in every upstream TU
        "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/src/esp_raylib_memory.h"
        -fno-strict-aliasing
        # Upstream warnings we cannot fix in the submodule (rlsw.h/rlgl.h noise).
        # TODO: drop once fixed upstream.
        -Wno-unused-variable
        -Wno-unused-function
        -Wno-incompatible-pointer-types
        -Wno-unused-but-set-variable
        # raylib is self-contained and safe at -O2 regardless.
        -O2
    )
endforeach()
