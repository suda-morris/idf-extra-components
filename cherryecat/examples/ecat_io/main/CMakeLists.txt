idf_component_register(
    SRCS
        "ecat_io_example_main.c"

    INCLUDE_DIRS
        "."

    REQUIRES
        cherryecat
        esp_eth
        esp_event
        esp_timer
        esp_driver_gpio
)
