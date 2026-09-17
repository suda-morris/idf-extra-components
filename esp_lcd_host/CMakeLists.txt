set(srcs "src/esp_lcd_host.c"
         "src/esp_lcd_host_sdl_backend.c"
         "src/esp_lcd_host_screenshot.c")

# The generic panel operations come from the esp_lcd component on chip targets.
# On the POSIX host target that component is not available, so the fallback
# implementation is built instead. The dependency is evaluated here, before
# esp_lcd is expanded into the build.
idf_build_get_property(idf_target IDF_TARGET)
if(idf_target STREQUAL "linux")
    list(APPEND srcs "src/esp_lcd_host_panel_ops.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include" "src"
                       PRIV_INCLUDE_DIRS "src"
                       PRIV_REQUIRES hal)

# The generic esp_lcd component is the one that provides esp_lcd_panel_ops.h and
# the esp_lcd_panel_t interface. It does not build on the POSIX host target, so
# the type definitions are replicated in the header. Link against it whenever it
# is part of the build (any chip target), which is what makes this panel behave
# exactly like a real LCD driver there.
idf_component_optional_requires(INTERFACE esp_lcd)
