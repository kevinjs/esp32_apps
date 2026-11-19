#include "xz_wifi.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"
#include "wifi_config.h"

static const char *TAG = "xz.wifi";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAILED_BIT     BIT1

static EventGroupHandle_t s_events = NULL;
static bool s_started = false;
static int s_retry = 0;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry < CONFIG_XZ_WIFI_MAX_RETRY) {
            s_retry++;
            ESP_LOGW(TAG, "disconnected, retry %d/%d", s_retry, CONFIG_XZ_WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "connection failed after %d retries", s_retry);
            xEventGroupSetBits(s_events, WIFI_FAILED_BIT);
        }
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry = 0;
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t xz_wifi_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    /* 凭证优先取 selector 保存的 NVS 配置，回落到编译期 Kconfig */
    char ssid[WIFI_CRED_SSID_MAX] = {0};
    char pass[WIFI_CRED_PASS_MAX] = {0};
    wifi_cred_t cred;
    if (wifi_config_load(&cred) == ESP_OK) {
        strlcpy(ssid, cred.ssid, sizeof(ssid));
        strlcpy(pass, cred.pass, sizeof(pass));
        ESP_LOGI(TAG, "using credentials saved in selector");
    } else if (strlen(CONFIG_XZ_WIFI_SSID) > 0) {
        strlcpy(ssid, CONFIG_XZ_WIFI_SSID, sizeof(ssid));
        strlcpy(pass, CONFIG_XZ_WIFI_PASSWORD, sizeof(pass));
        ESP_LOGW(TAG, "no saved credentials, falling back to Kconfig");
    } else {
        ESP_LOGE(TAG, "no wifi credentials: open selector -> WIFI and save one");
        return ESP_ERR_INVALID_STATE;
    }

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
                        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
                        IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = pass[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "connecting to \"%s\" ...", ssid);
    s_started = true;
    return ESP_OK;
}

bool xz_wifi_is_connected(void)
{
    if (s_events == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_events) & WIFI_CONNECTED_BIT) != 0;
}

esp_err_t xz_wifi_wait_connected(uint32_t timeout_ms)
{
    if (s_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events,
                                           WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    if (bits & WIFI_FAILED_BIT) {
        return ESP_FAIL;
    }
    return ESP_ERR_TIMEOUT;
}
