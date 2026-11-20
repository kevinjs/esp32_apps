#include "watch_time.h"

#include <stdlib.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "wifi_config.h"

static const char *TAG = "watch.time";

#define CLOCK_TZ        "CST-8"
#define NVS_NAMESPACE   "watch"
#define KEY_LAST_UTC    "utc"
#define TIME_EPOCH_MIN  1600000000      /* 2020-09-13 之后才算有效时间 */
#define SAVE_INTERVAL_S 300             /* 运行中每 5 分钟回写一次 NVS */

static volatile bool s_valid = false;

static bool time_plausible(void)
{
    return time(NULL) > TIME_EPOCH_MIN;
}

static void save_utc(void)
{
    const time_t now = time(NULL);
    if (now <= TIME_EPOCH_MIN) {
        return;
    }

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    nvs_set_i64(nvs, KEY_LAST_UTC, (int64_t)now);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static void restore_utc(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    int64_t saved = 0;
    if (nvs_get_i64(nvs, KEY_LAST_UTC, &saved) == ESP_OK && saved > TIME_EPOCH_MIN) {
        struct timeval tv = { .tv_sec = (time_t)saved, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "restored last time: %lld", (long long)saved);
    }
    nvs_close(nvs);
}

static bool wait_for_network(uint32_t timeout_ms)
{
    for (uint32_t waited = 0; waited < timeout_ms; waited += 200) {
        if (wifi_config_get_state() == WIFI_CFG_STATE_CONNECTED) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return false;
}

static void time_task(void *arg)
{
    (void)arg;

    if (!wait_for_network(30000)) {
        ESP_LOGW(TAG, "no Wi-Fi, running on last saved time");
        s_valid = time_plausible();
        vTaskDelete(NULL);
        return;
    }

    const esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed");
        vTaskDelete(NULL);
        return;
    }

    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) == ESP_OK && time_plausible()) {
        s_valid = true;
        save_utc();

        time_t now = time(NULL);
        struct tm local = { 0 };
        localtime_r(&now, &local);
        ESP_LOGI(TAG, "synced: %04d-%02d-%02d %02d:%02d:%02d",
                 local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                 local.tm_hour, local.tm_min, local.tm_sec);
    } else {
        ESP_LOGW(TAG, "SNTP timeout, using restored time");
        s_valid = time_plausible();
    }

    /* 长时间开表：定期把当前时间写回 NVS，下次开机起点更准 */
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(SAVE_INTERVAL_S * 1000));
        save_utc();
    }
}

void watch_time_start(void)
{
    setenv("TZ", CLOCK_TZ, 1);
    tzset();

    restore_utc();

    if (xTaskCreate(time_task, "watch_time", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "failed to create time task");
    }
}

bool watch_time_get(struct tm *out)
{
    if (out == NULL) {
        return false;
    }

    const time_t now = time(NULL);
    localtime_r(&now, out);
    return s_valid || now > TIME_EPOCH_MIN;
}
