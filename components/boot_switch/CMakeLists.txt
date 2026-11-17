idf_component_register(
    SRCS "boot_switch.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_partition app_update esp_timer esp_driver_gpio
)
