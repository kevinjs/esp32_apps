/*
 * 小智应用的集中配置
 *
 * 音频参数必须与 xiaozhi 服务端协商结果保持一致：
 *   - 采样率 16000 Hz
 *   - 单声道
 *   - 16 bit
 *   - Opus 帧长 60 ms（hello 里声明的 frame_duration）
 */
#pragma once

#define XZ_AUDIO_SAMPLE_RATE        16000
#define XZ_AUDIO_CHANNELS           1
#define XZ_AUDIO_BITS_PER_SAMPLE    16

/* 一帧 60ms 的 PCM 字节数：16000 * 2 * 0.06 = 1920 */
#define XZ_OPUS_FRAME_MS            60
#define XZ_PCM_FRAME_BYTES          (XZ_AUDIO_SAMPLE_RATE * (XZ_AUDIO_BITS_PER_SAMPLE / 8) * XZ_OPUS_FRAME_MS / 1000)

/* Opus 编码参数 */
#define XZ_OPUS_BITRATE             24000
#define XZ_OPUS_COMPLEXITY          3

/* 播放队列深度（每项是一帧 Opus 包） */
#define XZ_PLAY_QUEUE_DEPTH         32

/* UI */
#define XZ_UI_WIDTH                 466
#define XZ_UI_HEIGHT                466
