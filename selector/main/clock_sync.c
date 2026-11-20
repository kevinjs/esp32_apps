/*
 * SNTP 校时
 *
 * 时序：app_main → clock_sync_start() 起任务 → 等 wifi_config 连上 →
 * 起 SNTP（阿里云 + pool.ntp.org）→ sync_wait → 置 s_valid。
 * 校时期间 UI 显示 "--:--"，失败也不阻塞任何东西。
 */
#include "clock_sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi_config.h"

static const char *TAG = "selector.clock";

/* 东八区。以后做设置页时改成 NVS 里的可配置项 */
#define CLOCK_TZ     "CST-8"
/* 2020-09-13 之后的时间才算“已同步”，用来区分 1970 默认值 */
#define TIME_EPOCH_MIN 1600000000

static volatile bool s_valid = false;

static bool time_plausible(void)
{
    return time(NULL) > TIME_EPOCH_MIN;
}

static bool wait_for_network(uint32_t timeout_ms)
{
    for (uint32_t waited = 0; waited < timeout_ms; waited += 100) {
        if (wifi_config_get_state() == WIFI_CFG_STATE_CONNECTED) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

static void sync_task(void *arg)
{
    (void)arg;

    if (!wait_for_network(30000)) {
        ESP_LOGW(TAG, "no Wi-Fi, clock stays unsynced");
        vTaskDelete(NULL);
        return;
    }

    /* CONFIG_LWIP_SNTP_MAX_SERVERS 默认 1，这里只配一个；换服务器改这一行 */
    const esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) == ESP_OK && time_plausible()) {
        s_valid = true;
        time_t now = time(NULL);
        struct tm local = { 0 };
        localtime_r(&now, &local);
        ESP_LOGI(TAG, "time synced: %04d-%02d-%02d %02d:%02d",
                 local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                 local.tm_hour, local.tm_min);
    } else {
        ESP_LOGW(TAG, "SNTP sync timeout, clock stays unsynced");
    }

    vTaskDelete(NULL);
}

void clock_sync_start(void)
{
    setenv("TZ", CLOCK_TZ, 1);
    tzset();

    if (xTaskCreate(sync_task, "clock_sync", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "failed to create SNTP task");
    }
}

bool clock_sync_valid(void)
{
    return s_valid && time_plausible();
}

bool clock_sync_hhmm(char *buf, size_t len)
{
    if (buf == NULL || len < 6) {
        return false;
    }

    if (!clock_sync_valid()) {
        snprintf(buf, len, "--:--");
        return false;
    }

    const time_t now = time(NULL);
    struct tm local = { 0 };
    localtime_r(&now, &local);
    snprintf(buf, len, "%02d:%02d", local.tm_hour, local.tm_min);
    return true;
}
