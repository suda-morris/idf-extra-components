# Build the component: an esp_lcd panel driver backed by the SDL3 submodule.
# It only builds for the ESP-IDF host (linux) target.
#
# SDL3 is vendored as the submodule esp_lcd_host/SDL and built by the port in
# port/sdl/CMakeLists.txt with the CMake project of SDL itself. It is not taken
# from the component registry, because the port only has to work on the host
# and this way it can build SDL with the subsystems the LCD driver needs, and
# without the board support packages of the registry component. See
# port/sdl/CMakeLists.txt.
add_subdirectory("port/sdl")

idf_component_register(SRCS "src/esp_lcd_host.c"
                            "src/esp_lcd_host_panel_ops.c"
                            "src/esp_lcd_host_screenshot.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       PRIV_REQUIRES esp_lcd)

target_link_libraries(${COMPONENT_LIB} PRIVATE SDL3::SDL3)
