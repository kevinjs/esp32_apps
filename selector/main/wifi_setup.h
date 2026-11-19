/*
 * Wi-Fi 设置页（selector 专用）
 *
 * 交互：扫描附近 AP → 点选 → 屏幕键盘输入密码 → 存入 NVS（wifi_config 组件）。
 * 所有 app 启动时从同一份 NVS 读取凭证。
 */
#pragma once

#include "esp_err.h"

/**
 * @brief 打开设置页
 *
 * @note 必须在已持有 LVGL 锁的上下文中调用（例如 LVGL 事件回调里）。
 */
void wifi_setup_open(void);
