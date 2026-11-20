/*
 * 跨 app 的通用设置（NVS 持久化）
 *
 * selector 负责改，app 负责读：同一个 NVS namespace 下存一份，
 * 这样在 selector 里调的音量，进 xiaozhi / gmeter 后仍然生效。
 *
 * 目前只有扬声器音量；后续加（亮度、自动进上次 app、息屏时间…）都在这里扩展。
 */
#pragma once

#include "esp_err.h"

/** 默认音量（0..100），与 xiaozhi 的 Kconfig 默认值保持一致 */
#define APP_SETTINGS_DEFAULT_VOLUME  60

/** @brief 打开 NVS namespace；失败时所有 get 返回默认值、set 直接返回错误 */
esp_err_t app_settings_init(void);

/** @brief 扬声器音量 0..100（未设置时返回 APP_SETTINGS_DEFAULT_VOLUME） */
int app_settings_get_speaker_volume(void);

/** @brief 写入并持久化扬声器音量；入参会被夹到 0..100 */
esp_err_t app_settings_set_speaker_volume(int volume);
