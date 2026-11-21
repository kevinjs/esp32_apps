/*
 * 极简 DLNA/UPnP MediaRenderer（只做 QQ 音乐 QPlay 用到的那部分）
 *
 * 为什么自己写而不是用现成库：
 *   - 实测手机（iOS QQ 音乐 / QPlay v1）只用 8 个 SOAP 动作：见 dlna_renderer.c
 *     的 action 表，不需要 ContentDirectory、不需要完整 UPnP 栈；
 *   - 现成 C 实现（gmediarender 等）依赖 GStreamer/libupnp，ESP32 上不现实。
 *
 * 分工：
 *   - 本组件只负责"协议"：SSDP 发现、设备描述、SOAP 控制、GENA 订阅应答；
 *   - 音频（HTTP 拉流、解码、I2S）在 app 侧，组件通过 on_cmd 回调下发命令。
 *   - 手机高频轮询 GetTransportInfo/GetPositionInfo/GetMediaInfo（实测 5 分钟
 *     各 70+ 次），组件直接读共享状态回答，不打扰 app。
 *
 * 线程：回调在 HTTP 服务任务里执行，必须快速返回（建议只投递队列）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DLNA_TRANSPORT_NO_MEDIA = 0,    /* 还没收到过 URI */
    DLNA_TRANSPORT_STOPPED,
    DLNA_TRANSPORT_PLAYING,
    DLNA_TRANSPORT_PAUSED,
} dlna_transport_t;

typedef enum {
    DLNA_CMD_SET_URI = 0,   /* 已收到新 URI，arg 无意义（从回调参数 uri 取） */
    DLNA_CMD_PLAY,          /* 开始 / 从暂停位置继续 */
    DLNA_CMD_PAUSE,
    DLNA_CMD_STOP,
    DLNA_CMD_SEEK,          /* value = 目标位置 ms（REL_TIME） */
    DLNA_CMD_SET_VOLUME,    /* value = 0..100 */
} dlna_cmd_t;

/** @brief DIDL-Lite 里能拿到的曲目信息（手机推送时一起发过来） */
typedef struct {
    bool     has_media;
    char     title[96];
    char     artist[64];
    char     album[64];
    char     mime[32];      /* protocolInfo 里的 mime，注意 QPlay 会把 FLAC 标成 audio/x-ogg，不可信 */
    char     album_art[192];/* DIDL 里的 upnp:albumArtURI（手机不给就是空串） */
    uint32_t duration_ms;
} dlna_media_info_t;

/**
 * @brief 命令回调。
 *
 * @param cmd   动作
 * @param uri   DLNA_CMD_SET_URI 时有效（已做 XML 反转义，可直接 GET）
 * @param value SEEK 的毫秒 / SET_VOLUME 的音量，其它为 0
 */
typedef void (*dlna_cmd_cb_t)(dlna_cmd_t cmd, const char *uri, uint32_t value, void *user);

typedef struct {
    const char   *friendly_name;    /* 手机上显示的名字，例 "ESP32-DLNA" */
    const char   *model_name;       /* 设备描述里的 modelName */
    uint16_t      port;             /* HTTP 端口，QPlay 实测用 8090 没问题 */
    dlna_cmd_cb_t on_cmd;
    void         *user;
} dlna_renderer_cfg_t;

/** @brief 起 SSDP + HTTP 服务；必须在 WiFi 拿到 IP 之后调用。 */
esp_err_t dlna_renderer_start(const dlna_renderer_cfg_t *cfg);

/**
 * @brief 由播放器回报状态，供手机轮询使用。
 *
 * 位置/时长要真实：手机进度条、拖动 Seek 的目标都依赖它（旧探针返回 0:00
 * 导致"拖到中间却从 8 秒开始播"）。
 */
void dlna_renderer_set_transport(dlna_transport_t state, uint32_t position_ms, uint32_t duration_ms);

/** @brief 当前曲目信息（界面用）。没有媒体时 has_media=false。 */
void dlna_renderer_get_media(dlna_media_info_t *out);

/** @brief 手机看到的设备 UUID（URL/日志用）。 */
const char *dlna_renderer_uuid(void);

/** @brief 实际监听端口（cfg.port 为 0 时由系统分配，这里能查到）。 */
uint16_t dlna_renderer_port(void);

#ifdef __cplusplus
}
#endif
