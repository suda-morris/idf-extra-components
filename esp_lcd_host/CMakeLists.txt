# Build the component: an esp_lcd panel driver backed by the SDL3 submodule.
# It only builds for the ESP-IDF host (linux) target.
#
# SDL3 is vendored as the submodule esp_lcd_host/SDL and built by the port in
# port/sdl/CMakeLists.txt with the CMake project of SDL itself. It is not taken
# from the component registry, because the port only has to work on the host
# and this way it can build SDL with the subsystems the LCD driver needs, and
# without the board support packages of the registry component. See
# port/sdl/CMakeLists.txt.
# The SDL submodule is added as a CMake subdirectory. `add_subdirectory()` is
# not available in script mode, and the build system includes this file in
# script mode once to collect the component requirements before the real build,
# so only add it (and the target that it defines) in the real build.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    add_subdirectory("port/sdl")
endif()

idf_component_register(SRCS "src/esp_lcd_host.c"
                            "src/esp_lcd_host_panel_ops.c"
                            "src/esp_lcd_host_screenshot.c"
                            "src/esp_lcd_host_sdl_backend.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       PRIV_REQUIRES esp_lcd)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    target_link_libraries(${COMPONENT_LIB} PRIVATE SDL3::SDL3)
endif()
