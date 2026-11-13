set(CHERRYECAT_ROOT "${CMAKE_CURRENT_LIST_DIR}/cherryecat")

# Vendored upstream sources (src/, osal/).
set(CHERRYECAT_VENDORED_SOURCES
    ${CHERRYECAT_ROOT}/src/ec_coe.c
    ${CHERRYECAT_ROOT}/src/ec_common.c
    ${CHERRYECAT_ROOT}/src/ec_datagram.c
    ${CHERRYECAT_ROOT}/src/ec_eoe.c
    ${CHERRYECAT_ROOT}/src/ec_foe.c
    ${CHERRYECAT_ROOT}/src/ec_mailbox.c
    ${CHERRYECAT_ROOT}/src/ec_master.c
    ${CHERRYECAT_ROOT}/src/ec_netdev.c
    ${CHERRYECAT_ROOT}/src/ec_sii.c
    ${CHERRYECAT_ROOT}/src/ec_slave.c
    ${CHERRYECAT_ROOT}/src/ec_slave_table.c
    ${CHERRYECAT_ROOT}/src/ec_timestamp.c
    ${CHERRYECAT_ROOT}/src/phy/chry_phy.c
    ${CHERRYECAT_ROOT}/osal/ec_osal_freertos.c
)

# ESP-IDF port layer.
set(CHERRYECAT_PORT_SOURCES
    port/netdev_esp.c
    port/ec_htimer_esp.c
    port/ec_timestamp_esp.c
)

if(CONFIG_CHERRYECAT_CMD_ENABLE)
    list(APPEND CHERRYECAT_VENDORED_SOURCES
        ${CHERRYECAT_ROOT}/src/ec_cmd.c
    )
endif()

set(CHERRYECAT_SOURCES
    ${CHERRYECAT_VENDORED_SOURCES}
    ${CHERRYECAT_PORT_SOURCES}
)

# Upstream targets MCU toolchains where uint32_t is `unsigned int`; the ESP
# toolchains typedef it to `long unsigned int`, which trips -Werror=format on
# printf("%u") sites. Keep the vendored sources pristine and only downgrade
# the format warning there; the port layer stays strict.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set_source_files_properties(${CHERRYECAT_VENDORED_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-Wno-error=format;-Wno-error=missing-field-initializers"
    )
endif()

idf_component_register(
    SRCS ${CHERRYECAT_SOURCES}

    INCLUDE_DIRS
        "port/include"
        "${CHERRYECAT_ROOT}/include"

    # Include shim so the vendored FreeRTOS OSAL keeps its upstream
    # `#include <FreeRTOS.h>` style.
    PRIV_INCLUDE_DIRS
        "port/freertos_compat"

    REQUIRES
        esp_eth

    PRIV_REQUIRES
        esp_timer
        esp_event
        esp_netif
        esp_driver_gptimer
)
