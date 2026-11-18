/*
 * 小智语音助手（原生 ESP-IDF 实现）
 *
 * 与 ride-gmeter 平级：作为 ota_0 里的一个普通 app，由 selector 安装启动，
 * 长按 BOOT 3 秒回到 app 选择界面。
 *
 * 启动流程：
 *   1. NVS / boot_switch
 *   2. LVGL 界面（首帧出来即标记启动成功）
 *   3. Wi-Fi STA 连接
 *   4. 音频通路 + Opus
 *   5. esp_xiaozhi 协议层（首次需要激活码绑定）
 *   6. 点屏幕 TALK 按钮开始/停止一轮对话
 */
#include <stdbool.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "boot_switch.h"
#include "xz_audio.h"
#include "xz_chat.h"
#include "xz_ui.h"
#include "xz_wifi.h"

static const char *TAG = "xiaozhi";

typedef enum {
    XZ_CMD_TOGGLE = 0,
} xz_cmd_t;

static QueueHandle_t s_cmd_queue = NULL;

/* -------------------------------------------------------------------------- */

static void ui_button_cb(void)
{
    const xz_cmd_t cmd = XZ_CMD_TOGGLE;
    if (s_cmd_queue != NULL) {
        xQueueSend(s_cmd_queue, &cmd, 0);
    }
}

static void audio_capture_cb(const uint8_t *opus, size_t len, void *ctx)
{
    (void)ctx;
    xz_chat_send_audio(opus, len);
}

/* -------------------------------------------------------------------------- */

static void app_task(void *arg)
{
    (void)arg;

    xz_ui_set_state("WIFI");
    xz_ui_set_detail("connecting");

    esp_err_t err = xz_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(err));
        xz_ui_set_state("NO WIFI CONFIG");
        xz_ui_set_detail("set CONFIG_XZ_WIFI_SSID in menuconfig");
        goto idle;
    }
    err = xz_wifi_wait_connected(20000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi connect failed: %s", esp_err_to_name(err));
        xz_ui_set_state("NO WIFI");
        xz_ui_set_detail("check SSID / password");
        goto idle;
    }

    xz_ui_set_state("AUDIO");
    err = xz_audio_init(audio_capture_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed: %s", esp_err_to_name(err));
        xz_ui_set_state("AUDIO ERROR");
        goto idle;
    }

    xz_ui_set_state("LINK");
    err = xz_chat_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "chat init failed: %s", esp_err_to_name(err));
        xz_ui_set_state("SERVER ERROR");
        goto idle;
    }

    xz_ui_set_state("IDLE");
    xz_ui_set_detail("tap TALK to start");

idle:
    while (true) {
        xz_cmd_t cmd;
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (cmd != XZ_CMD_TOGGLE) {
            continue;
        }
        if (xz_chat_is_active()) {
            xz_chat_stop_session();
        } else {
            xz_chat_start_session();
        }
    }
}

/* -------------------------------------------------------------------------- */

void app_main(void)
{
    /* USB-Serial-JTAG 控制台的 stdout 默认全缓冲，显式改为行缓冲 */
    setvbuf(stdout, NULL, _IOLBF, 0);

    ESP_LOGI(TAG, "=== xiaozhi / native esp-idf ===");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(err));
    }

    /* 长按 BOOT 3 秒回 selector；连续启动失败也会自动回退 */
    boot_switch_config_t boot_cfg = BOOT_SWITCH_CONFIG_DEFAULT();
    boot_cfg.boot_key_gpio = CONFIG_XZ_BOOT_BUTTON_GPIO;
    boot_switch_init(&boot_cfg);

    s_cmd_queue = xQueueCreate(4, sizeof(xz_cmd_t));

    if (xz_ui_start(ui_button_cb) != ESP_OK) {
        ESP_LOGE(TAG, "UI start failed, running headless");
    }

    /* 界面已经出来，本次启动算成功，清零失败计数 */
    boot_switch_mark_boot_ok();

    BaseType_t created = xTaskCreate(app_task, "xz_app", 8192, NULL, 5, NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "failed to create app task");
    }
}
