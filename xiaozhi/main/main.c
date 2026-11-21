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
 *   6. 屏幕底部按钮（文案随状态变）：
 *      说话 -> 说完 -> 识别中 -> 播报(可打断) -> 说话 ...
 *      manual 模式必须发 listen stop，服务端才会开始识别
 */
#include <stdbool.h>
#include <stdio.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
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

/* 内部 RAM（尤其 DMA 可用部分）不够时 LVGL 刷屏和 mbedtls 会静默失败，逐阶段打点 */
static void log_heap(const char *stage)
{
    ESP_LOGI(TAG, "[heap] %-8s internal=%u dma=%u dma_max=%u int_min=%u psram=%u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/*
 * 常驻打点：多轮对话播报时 esp-aes 每个音频包都要在内部 DMA 内存里申请临时缓冲，
 * 一旦 dma_max（最大连续块）见底就解不出包、会话再也接不上。
 * 每 5 秒打一行，用来区分"内存被吃光"和"只是碎片化"。
 */
static void heap_monitor_task(void *arg)
{
    (void)arg;
    while (true) {
        log_heap("run");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* -------------------------------------------------------------------------- */

static void app_task(void *arg)
{
    (void)arg;

    log_heap("start");

    xz_ui_set_state("WIFI");
    xz_ui_set_detail("正在连接 Wi-Fi ...");

    esp_err_t err = xz_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(err));
        xz_ui_set_state("NO WIFI");
        xz_ui_set_detail("长按 BOOT 3 秒回应用列表重新配网");
        goto idle;
    }
    err = xz_wifi_wait_connected(20000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi connect failed: %s", esp_err_to_name(err));
        xz_ui_set_state("NO WIFI");
        xz_ui_set_detail("请检查 Wi-Fi 名称和密码");
        goto idle;
    }

    xz_ui_set_state("AUDIO");
    err = xz_audio_init(audio_capture_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed: %s", esp_err_to_name(err));
        xz_ui_set_state("AUDIO ERROR");
        goto idle;
    }
    log_heap("audio");

    xz_ui_set_state("LINK");
    err = xz_chat_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "chat init failed: %s", esp_err_to_name(err));
        xz_ui_set_state("SERVER ERROR");
        goto idle;
    }
    log_heap("chat");

    xz_ui_set_state("IDLE");
    xz_ui_set_detail("点下方按钮开始说话");

idle:
    while (true) {
        xz_cmd_t cmd;
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (cmd != XZ_CMD_TOGGLE) {
            continue;
        }
        if (!xz_chat_is_active()) {
            xz_chat_start_session();     /* TAP 1：连接 + 开通道 + 开始说话 */
        } else if (xz_chat_is_listening()) {
            xz_chat_talk_stop();         /* TAP 2：说完了，等服务端识别 */
        } else if (xz_chat_is_thinking()) {
            xz_chat_talk_cancel();       /* 等太久不想等了，直接放弃本次 */
        } else if (xz_chat_is_speaking() || xz_audio_is_busy()) {
            xz_chat_abort_speaking();    /* TAP：打断播报 */
        } else {
            xz_chat_talk_start();        /* TAP：再说一句 */
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

    if (xTaskCreate(heap_monitor_task, "xz_heap", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGW(TAG, "heap monitor task create failed");
    }
}
