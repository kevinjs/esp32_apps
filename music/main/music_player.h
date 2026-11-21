/*
 * 播放器：两路音源，同一个解码器 + 同一个喇叭
 *   - 本地：storage 里的 music-*.wav（循环播放，开机即播）
 *   - 投屏：DLNA 推过来的 HTTP 流（MP3 / FLAC），按 Range 拉取，可拖动进度
 *
 * 线程模型：只有一个播放任务碰解码器、文件和 HTTP 连接。
 * 界面/按键/手机控制都只投递命令或读快照，不会并发访问硬件。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MUSIC_STATE_INIT = 0,   /* 还没初始化完 */
    MUSIC_STATE_PLAYING,
    MUSIC_STATE_PAUSED,
    MUSIC_STATE_STOPPED,    /* 投屏流放完了（本地曲目是循环播放，不会进这个状态） */
    MUSIC_STATE_NO_TRACK,   /* storage 里没有 music-*.wav */
    MUSIC_STATE_ERROR,      /* 打开失败 / 格式不支持 / 解码失败 */
} music_state_t;

/** 声音从哪来：本地 storage 还是手机投屏 */
typedef enum {
    MUSIC_SOURCE_LOCAL = 0,
    MUSIC_SOURCE_DLNA,
} music_source_t;

typedef struct {
    music_state_t  state;
    music_source_t source;
    char     track[64];         /* 本地：文件名；投屏：歌名 */
    char     artist[64];        /* 投屏：歌手（本地为空） */
    uint32_t sample_rate;
    uint8_t  channels;
    uint8_t  bits;
    uint32_t position_ms;
    uint32_t duration_ms;
    int      volume;            /* 0..100 */
    char     error[64];         /* state == ERROR 时的原因 */
} music_status_t;

/**
 * @brief 初始化存储与音频通路，找到第一首 music-*.wav 后自动开始播放。
 *
 * 失败时不会返回错误码以外的副作用：状态会变成 NO_TRACK / ERROR，
 * 界面照样能起来（可以在屏幕上看到原因）。
 */
esp_err_t music_player_init(void);

/** @brief 播放 <-> 暂停。没有曲目或出错时返回 ESP_ERR_INVALID_STATE。 */
esp_err_t music_player_toggle(void);

/* -------------------------------------------------------------------------- */
/*  DLNA 投屏控制（可在任意任务调用：只往播放任务投递命令，立即返回）          */
/* -------------------------------------------------------------------------- */

/**
 * @brief 手机推来新地址：切到网络音源并从头准备（真正出声等 music_player_resume）。
 *
 * @param url         当前曲目 URL（http，QQ 音乐 CDN）
 * @param title       歌名，可为 NULL
 * @param artist      歌手，可为 NULL
 * @param duration_ms DIDL 里的时长；0 表示未知（进度条靠累计解码时长）
 */
esp_err_t music_player_play_url(const char *url, const char *title, const char *artist,
                                uint32_t duration_ms);

/** @brief 开始/继续（本地或投屏）。 */
esp_err_t music_player_resume(void);

/** @brief 暂停（保留连接，恢复时从原位置继续）。 */
esp_err_t music_player_pause(void);

/** @brief 停止投屏并回到本地曲目（暂停态，用户点按钮可继续本地播放）。 */
esp_err_t music_player_stop(void);

/** @brief 跳转到 position_ms（投屏流，按字节偏移重新 Range 拉流）。 */
esp_err_t music_player_seek(uint32_t position_ms);

/** @brief 设置音量 0..100（手机 RenderingControl 用；不写 NVS）。 */
esp_err_t music_player_set_volume(int volume);

/** @brief 取当前状态快照（任意任务可调，内部只做一次短临界区拷贝）。 */
void music_player_status(music_status_t *out);

/** @brief 状态中文名，给界面直接用。 */
const char *music_player_state_text(music_state_t state);

#ifdef __cplusplus
}
#endif
