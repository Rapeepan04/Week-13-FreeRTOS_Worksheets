idf_component_register(
    SRCS "esp32_freertos_advanced.c"
         "hardware_integration_example.c"
    INCLUDE_DIRS "."
    REQUIRES freertos sensor_manager
    PRIV_REQUIRES esp_timer driver
)