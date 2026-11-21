/*
 * DLNA/QPlay 投屏入口（QQ 音乐「投屏」能看到本机）
 *
 * 分工：
 *   - components/dlna_renderer 只管协议（SSDP/描述/SOAP/GENA）；
 *   - 本模块负责连 Wi-Fi、起 renderer、把手机命令转成播放器命令，
 *     并把播放器的真实进度回给手机（进度不准会让手机拖动算错目标）。
 *
 * 起完之后完全非阻塞：Wi-Fi 掉线/未配网时只是发现不了设备，本地播放不受影响。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 后台任务：等 Wi-Fi 拿到 IP → 起 renderer → 每 500 ms 同步播放状态。 */
esp_err_t music_dlna_start(void);

/** @brief 是否已经起来（已联网且 renderer 启动成功）。 */
bool music_dlna_ready(void);

/** @brief 手机要连的地址；未联网返回空串。 */
const char *music_dlna_ip(void);

#ifdef __cplusplus
}
#endif
