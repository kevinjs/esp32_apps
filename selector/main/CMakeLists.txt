idf_component_register(
    SRCS "main.c" "image_store.c" "image_install.c" "selector_ui.c"
    INCLUDE_DIRS "."
    REQUIRES boot_switch
    PRIV_REQUIRES spiffs app_update esp_app_format nvs_flash
)

# LVGL / BSP 由 main 组件隐式依赖，无需显式列出
