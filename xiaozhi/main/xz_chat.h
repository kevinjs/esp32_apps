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

/** 是否正在上行语音（已发 listen start，还没发 listen stop）。 */
bool xz_chat_is_listening(void);

/** 是否正在播报（服务端 tts 阶段）。 */
bool xz_chat_is_speaking(void);

/** 是否在等识别结果（已发 listen stop，8 秒超时后自动回 IDLE）。 */
bool xz_chat_is_thinking(void);

/** 放弃本次识别，立即回到 IDLE。 */
void xz_chat_talk_cancel(void);

/**
 * @brief 开始说话：通道已打开时发 listen start(manual) 并打开麦克风采集。
 * @note  服务端在 manual 模式下必须收到 listen stop 才会出识别结果。
 */
esp_err_t xz_chat_talk_start(void);

/** 说完一句：发 listen stop 并停止采集，等待 stt → tts 回来。 */
esp_err_t xz_chat_talk_stop(void);

/** 打断播报：通知服务端 abort 并清空本地播放队列。 */
void xz_chat_abort_speaking(void);

/** 上行一帧 Opus（由音频模块的采集回调调用）。 */
void xz_chat_send_audio(const uint8_t *data, size_t len);
