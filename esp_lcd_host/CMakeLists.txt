if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    add_subdirectory("port/sdl")
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
