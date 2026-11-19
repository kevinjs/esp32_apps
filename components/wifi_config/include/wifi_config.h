/*
 * 共享 Wi-Fi 凭证存储
 *
 * 所有 app（selector / ride-gmeter / xiaozhi / 以后的新 app）读写同一份配置：
 *   NVS namespace "wifi_cfg", keys "ssid" / "pass"
 *
 * 约定：
 *   - selector 是唯一写入方（设置页保存）
 *   - app 只读，不再各自用 menuconfig 硬编码账号
 *   - 换 app 不丢配置，除非手动清除
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_CRED_SSID_MAX  33
#define WIFI_CRED_PASS_MAX  65

typedef struct {
    char ssid[WIFI_CRED_SSID_MAX];
    char pass[WIFI_CRED_PASS_MAX];
} wifi_cred_t;

typedef struct {
    char   ssid[WIFI_CRED_SSID_MAX];
    int8_t rssi;
    bool   secure;
} wifi_ap_t;

/** 读取凭证。未保存过时返回 ESP_ERR_NOT_FOUND。 */
esp_err_t wifi_config_load(wifi_cred_t *out);

/** 保存凭证（自动 commit）。 */
esp_err_t wifi_config_save(const char *ssid, const char *pass);

/** 清除凭证。 */
esp_err_t wifi_config_clear(void);

/** 是否已经保存过非空 SSID。 */
bool wifi_config_has_credentials(void);

/* -------------------------------------------------------------------------- */
/* 后台自动连接（selector 与 app 通用）                                        */
/* -------------------------------------------------------------------------- */

typedef enum {
    WIFI_CFG_STATE_IDLE = 0,    /**< 还没发起连接 */
    WIFI_CFG_STATE_CONNECTING,  /**< 连接中 / 断线重试中 */
    WIFI_CFG_STATE_CONNECTED,   /**< 已拿到 IP */
    WIFI_CFG_STATE_FAILED,      /**< 没有凭证，或重试用尽 */
} wifi_config_state_t;

/**
 * @brief 按 NVS 里的凭证在后台自动连接（非阻塞）。
 *
 * 内部会懒初始化 netif/event loop/esp_wifi(STA)，断线自动重试
 * （最多 10 次，间隔 2 秒）。没有保存过凭证时返回 ESP_ERR_NOT_FOUND。
 */
esp_err_t wifi_config_autoconnect(void);

/** 当前连接状态。 */
wifi_config_state_t wifi_config_get_state(void);

/** 已连接时的 IPv4 字符串；未连接返回空串。 */
const char *wifi_config_get_ip(void);

/**
 * @brief 扫描附近 AP（阻塞，约 2～4 秒）
 *
 * 首次调用会懒初始化 netif / event loop / esp_wifi(STA)，只扫描不连接。
 * 结果按信号强度降序，同名 SSID 去重。
 */
esp_err_t wifi_config_scan(wifi_ap_t *out, int max_count, int *out_count);

#ifdef __cplusplus
}
#endif
