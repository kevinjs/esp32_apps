/*
 * 音乐播放器界面：渐变背景 + 曲目/格式/状态文字 + 进度条 + 播放按钮。
 *
 * 界面只读播放器快照，不碰解码器和文件；按钮点击只是 music_player_toggle()。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动显示与界面。首帧出来后调用方应立刻 boot_switch_mark_boot_ok()。
 */
esp_err_t music_ui_start(void);

#ifdef __cplusplus
}
#endif
