/*
 * 小智会话封装
 *
 * 只负责：拉取服务端配置、管理 chat 生命周期、把协议事件翻译成 UI 状态。
 * 音频编解码在 xz_audio，界面更新在 xz_ui。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** 初始化协议层（需 Wi-Fi 已连通）。成功后设备可随时开始对话。 */
esp_err_t xz_chat_init(void);

/** 开始一轮会话：连接服务器、打开音频通道、开始上行。 */
esp_err_t xz_chat_start_session(void);

/** 结束会话并断开。 */
esp_err_t xz_chat_stop_session(void);

/** 会话是否处于活动状态。 */
bool xz_chat_is_active(void);

/** 上行一帧 Opus（由音频模块的采集回调调用）。 */
void xz_chat_send_audio(const uint8_t *data, size_t len);
