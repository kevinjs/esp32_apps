idf_component_register(
    SRCS "cjk_font.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl
    PRIV_REQUIRES spiffs esp_partition heap
)
