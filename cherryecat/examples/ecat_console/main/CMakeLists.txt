idf_component_register(
    SRCS
        "ecat_console_example_main.c"

    INCLUDE_DIRS
        "."

    REQUIRES
        cherryecat
        esp_eth
        esp_event
        esp_timer
        console
        esp_stdio
        esp_driver_gpio
)
