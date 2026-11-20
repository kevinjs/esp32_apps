idf_component_register(
    SRCS
        "main.c"
        "watch_ui.c"
        "watch_time.c"
        "watch_stopwatch.c"
    INCLUDE_DIRS
        "."
    PRIV_REQUIRES
        esp_driver_gpio
        esp_netif      # esp_netif_sntp（watch_time.c）
        esp_timer
        nvs_flash      # 时间回写 / 读取（watch_time.c）
        boot_switch
        cjk_font
        tap_gesture
        ui_bg          # 与 selector 共用的斜向渐变背景
        wifi_config
)
