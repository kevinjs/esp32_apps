#include "wifi_config.h"

#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_config";

#define NVS_NAMESPACE   "wifi_cfg"
#define KEY_SSID        "ssid"
#define KEY_PASS        "pass"

#define AUTOCONNECT_MAX_RETRY   10
#define AUTOCONNECT_RETRY_MS    2000

static bool s_wifi_inited = false;

/* 自动连接状态：事件回调里写，UI 任务里读 */
static volatile wifi_config_state_t s_state = WIFI_CFG_STATE_IDLE;
static volatile bool s_autoconnect = false;
static uint8_t s_retry = 0;
static esp_timer_handle_t s_retry_timer = NULL;
static char s_ip[16] = "";

/* -------------------------------------------------------------------------- */

esp_err_t wifi_config_load(wifi_cred_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return err;
    }

    size_t len = sizeof(out->ssid);
    err = nvs_get_str(handle, KEY_SSID, out->ssid, &len);
    if (err != ESP_OK) {
        nvs_close(handle);
        return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_ERR_NOT_FOUND : err;
    }

    len = sizeof(out->pass);
    if (nvs_get_str(handle, KEY_PASS, out->pass, &len) != ESP_OK) {
        out->pass[0] = '\0';
    }

    nvs_close(handle);
    return out->ssid[0] != '\0' ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t wifi_config_save(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_PASS, pass != NULL ? pass : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "credentials saved for \"%s\"", ssid);
    } else {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t wifi_config_clear(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

bool wifi_config_has_credentials(void)
{
    wifi_cred_t cred;
    return wifi_config_load(&cred) == ESP_OK;
}

/* -------------------------------------------------------------------------- */

static void retry_timer_cb(void *arg)
{
    (void)arg;
    if (s_autoconnect) {
        ESP_LOGI(TAG, "retry connect (%u/%u)", (unsigned)s_retry, AUTOCONNECT_MAX_RETRY);
        esp_wifi_connect();
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_autoconnect) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ip[0] = '\0';
        if (s_autoconnect && s_retry < AUTOCONNECT_MAX_RETRY && s_retry_timer != NULL) {
            s_retry++;
            s_state = WIFI_CFG_STATE_CONNECTING;
            esp_timer_stop(s_retry_timer);
            esp_timer_start_once(s_retry_timer, AUTOCONNECT_RETRY_MS * 1000);
        } else {
            s_state = WIFI_CFG_STATE_FAILED;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        esp_ip4addr_ntoa(&ev->ip_info.ip, s_ip, sizeof(s_ip));
        s_retry = 0;
        s_state = WIFI_CFG_STATE_CONNECTED;
        ESP_LOGI(TAG, "connected, ip %s", s_ip);
    }
}

static esp_err_t wifi_lazy_init(void)
{
    if (s_wifi_inited) {
        return ESP_OK;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL) {
        esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }

    if (s_retry_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = retry_timer_cb,
            .name = "wifi_retry",
        };
        esp_timer_create(&timer_args, &s_retry_timer);
    }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        wifi_event_handler, NULL, NULL);

    err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    s_wifi_inited = true;
    return ESP_OK;
}

static int compare_rssi(const void *lhs, const void *rhs)
{
    const wifi_ap_t *a = (const wifi_ap_t *)lhs;
    const wifi_ap_t *b = (const wifi_ap_t *)rhs;
    return (int)b->rssi - (int)a->rssi;
}

esp_err_t wifi_config_scan(wifi_ap_t *out, int max_count, int *out_count)
{
    if (out == NULL || out_count == NULL || max_count <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_count = 0;

    esp_err_t err = wifi_lazy_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(err));
        return err;
    }

    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = {
            .active = {
                .min = 60,
                .max = 120,
            },
        },
    };

    err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan start failed: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num == 0) {
        return ESP_OK;
    }

    const uint16_t ask = ap_num > 32 ? 32 : ap_num;
    wifi_ap_record_t *records = calloc(ask, sizeof(wifi_ap_record_t));
    if (records == NULL) {
        return ESP_ERR_NO_MEM;
    }

    uint16_t got = ask;
    err = esp_wifi_scan_get_ap_records(&got, records);
    if (err != ESP_OK) {
        free(records);
        return err;
    }

    int count = 0;
    for (uint16_t i = 0; i < got && count < max_count; i++) {
        const char *ssid = (const char *)records[i].ssid;
        if (ssid[0] == '\0') {
            continue;   /* 隐藏 SSID */
        }

        bool duplicate = false;
        for (int j = 0; j < count; j++) {
            if (strcmp(out[j].ssid, ssid) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        strlcpy(out[count].ssid, ssid, sizeof(out[count].ssid));
        out[count].rssi = records[i].rssi;
        out[count].secure = records[i].authmode != WIFI_AUTH_OPEN;
        count++;
    }

    free(records);
    qsort(out, count, sizeof(out[0]), compare_rssi);
    *out_count = count;
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */

esp_err_t wifi_config_autoconnect(void)
{
    wifi_cred_t cred;
    const esp_err_t load_err = wifi_config_load(&cred);
    if (load_err != ESP_OK) {
        s_state = WIFI_CFG_STATE_FAILED;
        ESP_LOGW(TAG, "no credentials saved, skip auto connect");
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = wifi_lazy_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(err));
        s_state = WIFI_CFG_STATE_FAILED;
        return err;
    }

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, cred.ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, cred.pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;    /* 允许开放网络 */

    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set config failed: %s", esp_err_to_name(err));
        s_state = WIFI_CFG_STATE_FAILED;
        return err;
    }

    s_autoconnect = true;
    s_retry = 0;
    s_state = WIFI_CFG_STATE_CONNECTING;
    ESP_LOGI(TAG, "auto connecting to \"%s\"", cred.ssid);

    /* 已经连着就别打断；否则发起连接（重复调用只会返回已连接/进行中） */
    esp_err_t conn_err = esp_wifi_connect();
    if (conn_err != ESP_OK && conn_err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "connect request: %s", esp_err_to_name(conn_err));
    }
    return ESP_OK;
}

wifi_config_state_t wifi_config_get_state(void)
{
    return s_state;
}

const char *wifi_config_get_ip(void)
{
    return s_ip;
}
