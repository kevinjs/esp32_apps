/*
 * 音频通路：ES7210 录音 + ES8311 回放 + Opus 编解码
 *
 * 半双工：播放 TTS 时由上层调用 xz_audio_set_capture(false) 暂停上行，
 * 避免扬声器声音被麦克风收回造成自激。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*xz_audio_capture_cb_t)(const uint8_t *opus, size_t len, void *ctx);

/** 初始化 I2S、codec、Opus 编解码并启动播放任务。 */
esp_err_t xz_audio_init(xz_audio_capture_cb_t cb, void *ctx);

/** 打开/关闭录音上行（关闭时仍从 DMA 取数据并丢弃，避免溢出）。 */
esp_err_t xz_audio_set_capture(bool enable);

/** 投递一帧 Opus（来自服务端 TTS），内部拷贝。 */
esp_err_t xz_audio_play_opus(const uint8_t *data, size_t len);

/** 丢弃尚未播放的数据（打断说话时用）。 */
void xz_audio_flush(void);

/** 当前是否有待播放数据或正在播放。 */
bool xz_audio_is_busy(void);
