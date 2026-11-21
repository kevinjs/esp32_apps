idf_component_register(
    SRCS "main.c" "image_store.c" "image_install.c" "selector_ui.c" "wifi_setup.c"
         "settings_ui.c" "storage_ui.c" "battery.c" "clock_sync.c"
         "screen_power.c"
    INCLUDE_DIRS "."
    REQUIRES boot_switch wifi_config cjk_font app_settings tap_gesture ui_bg ui_scroll_fx
    PRIV_REQUIRES spiffs app_update esp_app_format nvs_flash esp_wifi esp_netif esp_driver_i2c
)

# LVGL / BSP 由 main 组件隐式依赖，无需显式列出
