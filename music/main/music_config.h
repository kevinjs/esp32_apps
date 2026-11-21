/*
 * music 可调参数集中在这里。
 *
 * 0.1.0 只支持一种格式：48 kHz / 16 bit / 单声道 PCM WAV
 * （I2S 与 ES8311 在启动时按这个采样率固定配置，换采样率要改代码重编）。
 */
#pragma once

/* ------------------------------ 音频 ------------------------------ */

#define MUSIC_SAMPLE_RATE           48000
#define MUSIC_CHANNELS              1
#define MUSIC_BITS_PER_SAMPLE       16

/** 一次从 storage 读多少压缩/原始数据（WAV 是 PCM，等价于一次读多少 PCM） */
#define MUSIC_READ_CHUNK            (8 * 1024)

/**
 * 解码输出块。
 * FLAC 一帧最大 = blocksize(4608~6144) × 声道 × 采样位宽，实测 QQ 音乐 SQ
 * 要 24576 字节，16 KB 会直接 ESP_AUDIO_ERR_BUFF_NOT_ENOUGH（手机侧表现为"播放失败"）。
 * 可以放 PSRAM：i2s_channel_write 会先把数据拷进内部 DMA 描述符再发。
 */
#define MUSIC_OUT_CHUNK             (32 * 1024)

/** 单个解码块最长占用的播放时间上限，用来给界面刷新兜底 */
#define MUSIC_UI_TICK_MS            200

/* ------------------------------ 存储 ------------------------------ */

#define MUSIC_STORAGE_LABEL         "storage"
#define MUSIC_STORAGE_BASE          "/storage"
#define MUSIC_TRACK_PREFIX          "music-"
#define MUSIC_TRACK_SUFFIX          ".wav"

/* ------------------------------ 按键 ------------------------------ */

/** BOOT 键 GPIO（本板 BOOT = GPIO0，低电平有效） */
#define MUSIC_BOOT_GPIO             0

/** 长按多久由 boot_switch 接管回 selector（ms） */
#define MUSIC_BOOT_SELECTOR_MS      3000

/* ------------------------------ 网络 / DLNA 投屏 ------------------------------ */

/** HTTP 拉流一次读多少字节（放 PSRAM；QQ 音乐 CDN 是普通 HTTP，非分片） */
#define MUSIC_NET_CHUNK             (8 * 1024)

/** 拉流超时：断网/链接过期（vkey 有有效期）时不要卡死播放任务 */
#define MUSIC_NET_TIMEOUT_MS        8000

/** 等拉流任务把连接建起来的超时（超时报错，别让播放任务干等） */
#define MUSIC_NET_OPEN_TIMEOUT_MS   10000

/** 手机 QPlay 设备列表里显示的名字 */
#define MUSIC_DLNA_NAME             "ESP32 音乐"

/** 播控命令队列深度：SetURI→Play 连发时不能丢 */
#define MUSIC_CMD_QUEUE_LEN         4

/** URL 上限：QQ 音乐的 vkey 链接实测 250~400 字节（与 dlna_renderer 的 URI_CAP 对齐） */
#define MUSIC_URL_MAX               640

/* ------------------------------ 界面 ------------------------------ */

#define MUSIC_COL_TEXT              0xF2F2F2    /* 与 selector 的 UI_COLOR_TEXT 一致 */
#define MUSIC_COL_MUTED             0x8A8A99    /* 与 selector 的 UI_COLOR_MUTED 一致 */
#define MUSIC_COL_TRACK             0x232333    /* 进度条轨道 */
#define MUSIC_COL_ACCENT            0x7C5CFF    /* selector 主色 */
#define MUSIC_COL_ACCENT_2          0x35D0FF    /* 主色渐变末端（青） */
#define MUSIC_COL_WARN              0xE5B454
#define MUSIC_COL_ERR               0xE74C3C
#define MUSIC_COL_CARD              0x181826
