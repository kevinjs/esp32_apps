/*
 * Wi-Fi STA 连接
 *
 * 账号来自 Kconfig（CONFIG_XZ_WIFI_SSID / CONFIG_XZ_WIFI_PASSWORD），
 * 先不做配网 UI：保持第一版链路简单。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** 启动 STA 并开始连接（非阻塞）。SSID 为空时返回 ESP_ERR_INVALID_STATE。 */
esp_err_t xz_wifi_start(void);

/** 当前是否已拿到 IP。 */
bool xz_wifi_is_connected(void);

/** 等待拿到 IP，超时返回 ESP_ERR_TIMEOUT。 */
esp_err_t xz_wifi_wait_connected(uint32_t timeout_ms);
