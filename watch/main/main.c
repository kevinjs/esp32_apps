/*
 * watch · 圆形表盘 + 秒表
 *
 * 启动顺序：
 *   1) boot_switch_init：连续失败启动兜底 + 长按 BOOT 回 selector
 *   2) Wi-Fi 后台连接（凭证来自 selector 的设置页），随后开始校时任务
 *   3) 显示 + 共享中文字库 + 界面
 *   4) BOOT 键轮询任务：短按开始/暂停，中等按住记圈
 *
 * 秒表状态与界面完全解耦：BOOT 键任务、屏幕按钮、LVGL 定时器都只通过
 * watch_stopwatch.h 里的四个函数交互。
 */
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/esp-bsp.h"

#include "app_settings.h"
#include "boot_switch.h"
#include "wifi_config.h"

#include "watch_config.h"
#include "watch_stopwatch.h"
#include "watch_time.h"
#include "watch_ui.h"

static const char *TAG = "watch";

/* -------------------------------------------------------------------------- */
/*  BOOT 键                                                                    */
/* -------------------------------------------------------------------------- */

/*
 * 超过 WATCH_BOOT_SELECTOR_MS 的长按由 boot_switch 自己的任务处理（直接重启），
 * 这里只判断"松开"时落在哪个时间窗，两边互不干扰地读同一个 GPIO。
 */
static void boot_key_task(void *arg)
{
    (void)arg;

    int last_level = 1;
    int64_t press_us = 0;

    while (true) {
        const int level = gpio_get_level((gpio_num_t)WATCH_BOOT_GPIO);

        if (level == 0 && last_level == 1) {
            press_us = esp_timer_get_time();
        } else if (level == 1 && last_level == 0) {
            const int64_t held_ms = (esp_timer_get_time() - press_us) / 1000;

            if (held_ms >= WATCH_BOOT_DEBOUNCE_MS && held_ms < WATCH_BOOT_TAP_MAX_MS) {
                /* 短按：开始 / 暂停，同时把界面拉到秒表页，按下就能看到反应 */
                watch_ui_show_stopwatch();
                watch_stopwatch_toggle();
                ESP_LOGI(TAG, "BOOT tap (%lld ms): toggle", (long long)held_ms);
            } else if (held_ms >= WATCH_BOOT_TAP_MAX_MS && held_ms < WATCH_BOOT_LAP_MAX_MS) {
                watch_ui_show_stopwatch();
                watch_stopwatch_lap();
                ESP_LOGI(TAG, "BOOT hold (%lld ms): lap", (long long)held_ms);
            }
        }

        last_level = level;
        vTaskDelay(pdMS_TO_TICKS(WATCH_KEY_POLL_MS));
    }
}

static esp_err_t boot_key_start(void)
{
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << WATCH_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(boot_key_task, "watch.boot", 3072, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */

void app_main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    ESP_LOGI(TAG, "=== watch: round face + stopwatch ===");

    /*
     * 启动守护尽早起：放在显示、联网之前，
     * 这样即使 UI 初始化失败，长按 BOOT 也能回到 selector。
     */
    boot_switch_config_t boot_cfg = BOOT_SWITCH_CONFIG_DEFAULT();
    boot_cfg.boot_key_gpio = WATCH_BOOT_GPIO;
    boot_cfg.boot_key_hold_ms = WATCH_BOOT_SELECTOR_MS;
    boot_switch_init(&boot_cfg);

    /* 联网：凭证由 selector 写入同一份 NVS，这里只读；失败就靠 NVS 里的上次时间 */
    if (wifi_config_autoconnect() != ESP_OK) {
        ESP_LOGW(TAG, "no Wi-Fi credentials, run selector -> Settings to configure");
    }
    watch_time_start();     /* 先从 NVS 恢复时间，联网成功后再 SNTP 校时 */

    if (watch_ui_start() != ESP_OK) {
        ESP_LOGE(TAG, "UI start failed, running headless (hold BOOT to return)");
        return;
    }

    /*
     * 亮度跟随 selector「设置 → 亮度」里存的值：app 每次启动都是全新初始化，
     * 不在这里套一次，用户调过的亮度进 app 就丢了。
     */
    bsp_display_brightness_set(app_settings_get_brightness());

    esp_err_t err = boot_key_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BOOT key task unavailable (%s), use on-screen buttons only",
                 esp_err_to_name(err));
    }

    /* 界面已经出来，本次启动算成功，清零失败计数 */
    boot_switch_mark_boot_ok();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
