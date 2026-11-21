/*
 * music · 48 kHz WAV 音乐播放器
 *
 * 启动顺序：
 *   1) NVS + boot_switch（长按 BOOT 3 秒回 selector、连续失败自动回退）
 *   2) 显示 + 共享中文字库 + 界面（首帧出来即标记启动成功）
 *   3) 播放器：挂 storage → 找 music-*.wav → simple decoder 解 WAV → ES8311 出声
 *
 * 与 xiaozhi / watch 平级：编译成普通 app 镜像，由 selector 装进 ota_0 启动。
 * 不要用 `idf.py flash`：它写 0x20000（factory），会把 selector 覆盖掉。
 */
#include <stdio.h>

#include "app_settings.h"
#include "bsp/esp-bsp.h"
#include "boot_switch.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "music_config.h"
#include "music_dlna.h"
#include "music_player.h"
#include "music_ui.h"

static const char *TAG = "music";

static void log_heap(const char *stage)
{
    ESP_LOGI(TAG, "[heap] %-6s internal=%u dma=%u dma_max=%u psram=%u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void app_main(void)
{
    /* USB-Serial-JTAG 控制台的 stdout 默认全缓冲，显式改为行缓冲 */
    setvbuf(stdout, NULL, _IOLBF, 0);

    ESP_LOGI(TAG, "=== music: %d Hz WAV player ===", MUSIC_SAMPLE_RATE);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(err));
    }

    boot_switch_config_t boot_cfg = BOOT_SWITCH_CONFIG_DEFAULT();
    boot_cfg.boot_key_gpio = MUSIC_BOOT_GPIO;
    boot_cfg.boot_key_hold_ms = MUSIC_BOOT_SELECTOR_MS;
    boot_switch_init(&boot_cfg);

    /* 界面先起来：即使音频/存储出问题，也能在屏幕上看到原因并长按 BOOT 退出 */
    if (music_ui_start() != ESP_OK) {
        ESP_LOGE(TAG, "UI start failed, running headless (hold BOOT to return)");
    } else {
        boot_switch_mark_boot_ok();
    }

    /* 亮度跟随 selector 设置页（app 每次启动都是全新初始化） */
    bsp_display_brightness_set(app_settings_get_brightness());

    err = music_player_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "player init failed: %s", esp_err_to_name(err));
    }

    /* 投屏是可选能力：没配网/连不上只是手机上发现不了设备，本地播放照常 */
    music_dlna_start();
    log_heap("ready");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
