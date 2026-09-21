idf_build_get_property(target IDF_TARGET)

set(srcs "src/clarke_park_f.c" "src/clarke_park_iq.c" "src/clarke_park_sincos.c")
set(priv_requires)

if(NOT "${target}" STREQUAL "linux")
    list(APPEND priv_requires "esp_driver_cordic")
endif()

if(CONFIG_SOC_CORDIC_SUPPORTED)
    list(APPEND srcs "src/clarke_park_cordic.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES ${priv_requires})
