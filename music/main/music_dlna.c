/*
 * DLNA 投屏胶水层
 *
 * 为什么要单独一个任务：
 *   手机发来的 SOAP 请求由 esp_http_server 的任务处理，那个任务里绝不能做
 *   拉流/解码（会阻塞手机轮询，QQ 音乐会以为设备卡死）。这里只是把命令塞进
 *   播放器的命令队列，然后立刻返回。
 *
 * 为什么不把 renderer 的回调直接接播放器：
 *   renderer 的 on_cmd 在 HTTP 任务里执行，play/pause 这类操作要落到唯一碰
 *   解码器和 I2S 的播放任务上，所以全部走队列（music_player_* 内部已加队列）。
 */
#include "music_dlna.h"

#include <stdio.h>
#include <string.h>

#include "esp_wifi.h"

#include "dlna_renderer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "music_config.h"
#include "music_player.h"
#include "wifi_config.h"

static const char *TAG = "music.dlna";

#define DLNA_TASK_STACK     4096
#define WIFI_RETRY_TICKS    60      /* FAILED 后每 500 ms 一个 tick，60 tick = 30 s 重试一次 */

static volatile bool s_ready = false;

/* 记住最近一次 DIDL 里的封面地址：SET_URI 只发一次，日志容易错过，
   所以在主循环里周期性再打一遍，方便确认手机到底给不给封面 */
static char s_album_art[192];

bool music_dlna_ready(void)
{
    return s_ready;
}

const char *music_dlna_ip(void)
{
    return wifi_config_get_ip();
}

/** @brief 播放器状态 -> UPnP TransportState */
static dlna_transport_t to_transport(music_state_t state)
{
    switch (state) {
    case MUSIC_STATE_PLAYING:
        return DLNA_TRANSPORT_PLAYING;
    case MUSIC_STATE_PAUSED:
        return DLNA_TRANSPORT_PAUSED;
    case MUSIC_STATE_STOPPED:
        return DLNA_TRANSPORT_STOPPED;
    case MUSIC_STATE_INIT:
    case MUSIC_STATE_NO_TRACK:
    case MUSIC_STATE_ERROR:
    default:
        return DLNA_TRANSPORT_NO_MEDIA;
    }
}

/**
 * @brief 把真实进度回给手机。
 *
 * 只在投屏音源时上报：本地放 WAV 时手机那边没有媒体，回 PLAYING 反而会让
 * QQ 音乐的进度条乱跳。
 */
static void sync_transport(void)
{
    music_status_t st;
    music_player_status(&st);
    if (st.source != MUSIC_SOURCE_DLNA) {
        return;
    }
    dlna_renderer_set_transport(to_transport(st.state), st.position_ms, st.duration_ms);
}

/** @brief renderer 回调（HTTP 任务上下文，必须立即返回） */
static void on_dlna_cmd(dlna_cmd_t cmd, const char *uri, uint32_t value, void *user)
{
    (void)user;

    switch (cmd) {
    case DLNA_CMD_SET_URI: {
        dlna_media_info_t info = { 0 };
        dlna_renderer_get_media(&info);
        ESP_LOGI(TAG, "phone push: %s - %s (%u ms)", info.artist, info.title,
                 (unsigned)info.duration_ms);
        snprintf(s_album_art, sizeof(s_album_art), "%s", info.album_art);
        music_player_play_url(uri, info.title, info.artist, info.duration_ms);
        break;
    }
    case DLNA_CMD_PLAY:
        music_player_resume();
        break;
    case DLNA_CMD_PAUSE:
        music_player_pause();
        break;
    case DLNA_CMD_STOP:
        music_player_stop();
        break;
    case DLNA_CMD_SEEK:
        music_player_seek(value);
        break;
    case DLNA_CMD_SET_VOLUME:
        music_player_set_volume((int)value);
        break;
    default:
        break;
    }
}

static bool start_renderer(void)
{
    const dlna_renderer_cfg_t cfg = {
        .friendly_name = MUSIC_DLNA_NAME,
        .model_name = "ESP32-S3 Music Renderer",
        .port = 8090,
        .on_cmd = on_dlna_cmd,
        .user = NULL,
    };

    const esp_err_t err = dlna_renderer_start(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "renderer start failed: %s", esp_err_to_name(err));
        return false;
    }

    s_ready = true;
    ESP_LOGI(TAG, "DLNA ready: \"%s\" @ %s:%u", MUSIC_DLNA_NAME, music_dlna_ip(),
             (unsigned)dlna_renderer_port());
    return true;
}

static void dlna_task(void *arg)
{
    (void)arg;

    if (wifi_config_autoconnect() != ESP_OK) {
        ESP_LOGW(TAG, "没有 Wi-Fi 凭证，投屏不可用（selector -> 设置 -> Wi-Fi）");
    }

    bool started = false;
    uint32_t failed_ticks = 0;

    while (true) {
        const wifi_config_state_t ws = wifi_config_get_state();

        if (!started) {
            if (ws == WIFI_CFG_STATE_CONNECTED) {
                /* 投屏是实时流：默认 WIFI_PS_MIN_MODEM 要等 DTIM 才收包，实测会给
                   单次 8 KB read 带来 50~150 ms 的周期性尖峰，正好把 I2S DMA 打空
                   （听感是偶尔"咔"一下）。播放器场景直接关掉省电。 */
                const esp_err_t ps = esp_wifi_set_ps(WIFI_PS_NONE);
                ESP_LOGI(TAG, "wifi power save off: %s", esp_err_to_name(ps));
                started = start_renderer();
                failed_ticks = 0;
            } else if (ws == WIFI_CFG_STATE_FAILED) {
                /* 组件内部重试 10 次就放弃，这里每 30 s 再拉一次，避免必须重启 app */
                if (++failed_ticks >= WIFI_RETRY_TICKS) {
                    failed_ticks = 0;
                    ESP_LOGI(TAG, "wifi retry");
                    wifi_config_autoconnect();
                }
            }
        }

        if (started) {
            sync_transport();

            /* 封面地址每 30 秒复报一次，避免只在 SET_URI 那一瞬的日志里出现 */
            static uint32_t last_art_log = 0;
            const uint32_t now = esp_log_timestamp();
            if (s_album_art[0] != '\0' && now - last_art_log >= 30000) {
                last_art_log = now;
                ESP_LOGI(TAG, "album art: %s", s_album_art);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

esp_err_t music_dlna_start(void)
{
    if (xTaskCreate(dlna_task, "music.dlna", DLNA_TASK_STACK, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
