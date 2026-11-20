/*
 * 启动选择器（factory 分区）
 *
 * 上电后 bootloader 启动本程序（otadata 为空或指向 factory 时）：
 *   1. 挂载 storage（SPIFFS），扫描 app-*.bin
 *   2. 显示列表，触摸选择
 *   3. 选中后把镜像写入 ota_0，切换启动分区并重启
 */
#include <stdbool.h>
#include <stddef.h>

#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"

#include "boot_switch.h"
#include "battery.h"
#include "cjk_font.h"
#include "clock_sync.h"
#include "image_install.h"
#include "image_store.h"
#include "screen_power.h"
#include "selector_ui.h"
#include "wifi_config.h"

static const char *TAG = "selector";

static image_entry_t s_images[IMAGE_STORE_MAX];
static int s_image_count = 0;

static void on_image_selected(const char *file)
{
    const esp_err_t err = image_install_start(file);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to start install: %s", esp_err_to_name(err));
    }
}

void app_main(void)
{
    /* selector 是恢复入口：NVS 异常时不擦除，避免把 IMU 标定一起清掉 */
    const esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err != ESP_OK) {
        ESP_LOGW(TAG, "NVS init failed (%s), calibration data left untouched", esp_err_to_name(nvs_err));
    }

    boot_switch_config_t cfg = BOOT_SWITCH_CONFIG_DEFAULT();
    cfg.watch_boot_key = false;     /* 已经运行在 factory，不需要按键兜底 */
    boot_switch_init(&cfg);

    /* 进选择器就连 Wi-Fi：后台连接不阻塞 UI，顶部图标随状态变色 */
    if (wifi_config_autoconnect() != ESP_OK) {
        ESP_LOGI(TAG, "no Wi-Fi credentials yet, use Settings -> Wi-Fi to configure");
    }

    /* 状态栏用的电池与时间；任一失败都只是显示占位符，不影响选 app */
    if (battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "battery unavailable, status bar keeps placeholder icon");
    } else if (battery_power_key_init() != ESP_OK) {
        ESP_LOGW(TAG, "power key unavailable, screen stays on");
    }
    clock_sync_start();

    size_t fs_total = 0;
    size_t fs_used = 0;
    const esp_err_t store_err = image_store_init();
    if (store_err == ESP_OK) {
        s_image_count = image_store_scan(s_images, IMAGE_STORE_MAX);
        image_store_info(&fs_total, &fs_used);
    } else {
        ESP_LOGE(TAG, "storage unavailable: %s", esp_err_to_name(store_err));
    }
    ESP_LOGI(TAG, "%d images, storage used %u/%u bytes",
             s_image_count, (unsigned)fs_used, (unsigned)fs_total);

    lv_display_t *display = bsp_display_start();
    if (display == NULL) {
        ESP_LOGE(TAG, "display init failed");
        return;
    }

    /* 记录亮度与触摸设备，供电源键切换息屏使用 */
    screen_power_init();

    /* 中文字库从 storage 读（selector 的 factory 分区放不下 2MB 字库） */
    if (cjk_font_init() != ESP_OK) {
        ESP_LOGW(TAG, "CJK font unavailable, UI falls back to Montserrat");
    }

    if (bsp_display_lock(1000) != ESP_OK) {
        ESP_LOGE(TAG, "timeout taking LVGL lock");
        return;
    }
    selector_ui_create(s_images, s_image_count, fs_total, fs_used, on_image_selected);
    bsp_display_unlock();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
