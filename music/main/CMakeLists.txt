idf_component_register(
    SRCS
        "main.c"
        "music_player.c"
        "music_ui.c"
        "music_dlna.c"
    INCLUDE_DIRS
        "."
    PRIV_REQUIRES
        boot_switch
        cjk_font
        ui_bg
        app_settings
        esp_audio_codec    # esp_audio_simple_dec：WAV/MP3/FLAC 容器解析 + PCM 解码
        esp_codec_dev      # ES8311 喇叭
        esp_driver_i2s
        esp_http_client    # 投屏流拉取（DLNA 推过来的 mp3/flac URL）
        spiffs             # storage 分区（读 music-*.wav）
        nvs_flash
        dlna_renderer      # DLNA/QPlay renderer（SSDP + SOAP + GENA）
        wifi_config        # 与 selector 共用的 Wi-Fi 凭证与自动连接
        esp_wifi           # 投屏时关掉 modem sleep（否则周期性收包延迟造成卡顿）
)
