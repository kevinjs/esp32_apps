idf_component_register(
    SRCS "main.c" "image_store.c" "image_install.c" "selector_ui.c" "wifi_setup.c"
    INCLUDE_DIRS "."
    REQUIRES boot_switch wifi_config cjk_font
    PRIV_REQUIRES spiffs app_update esp_app_format nvs_flash esp_wifi esp_netif
)

# LVGL / BSP 由 main 组件隐式依赖，无需显式列出
