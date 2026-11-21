#include "xz_chat.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mcp_engine.h"
#include "esp_timer.h"
#include "esp_xiaozhi_chat.h"
#include "esp_xiaozhi_info.h"

#include "xz_audio.h"
#include "xz_ui.h"

static const char *TAG = "xz.chat";

static esp_xiaozhi_chat_handle_t s_chat = 0;
static esp_mcp_t *s_mcp = NULL;
static volatile bool s_ready = false;
static volatile bool s_active = false;
static volatile bool s_channel_open = false;
static volatile bool s_listening = false;
static volatile bool s_speaking = false;
static volatile bool s_thinking = false;    /* 已发 listen stop，等服务端识别 */
static volatile uint32_t s_tx_frames = 0;   /* 本轮 listen 期间上传的 Opus 帧数 */

/* 服务端迟迟不回文本时的解脱时间：短按开始/立刻停止、或没说话就停，都会走到这里 */
#define THINK_TIMEOUT_MS    8000
static esp_timer_handle_t s_think_timer = NULL;

static void think_timer_cancel(void);

static void think_timeout_cb(void *arg)
{
    (void)arg;
    if (!s_thinking || s_listening || s_speaking) {
        return;
    }
    s_thinking = false;
    ESP_LOGW(TAG, "%d ms 没有识别结果，回到 IDLE", THINK_TIMEOUT_MS);
    xz_ui_set_state("IDLE");
    xz_ui_set_detail("没听清，再说一次");
}

/* -------------------------------------------------------------------------- */
/* 上行语音控制                                                               */

static esp_err_t chat_start_listening(void)
{
    if (!s_channel_open) {
        return ESP_ERR_INVALID_STATE;
    }
    /* manual：服务端不做 VAD，必须收到 listen stop 才会开始识别 */
    esp_err_t err = esp_xiaozhi_chat_send_start_listening(s_chat, ESP_XIAOZHI_CHAT_LISTENING_MODE_MANUAL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "listen start failed: %s", esp_err_to_name(err));
        return err;
    }
    think_timer_cancel();
    s_tx_frames = 0;
    s_listening = true;
    xz_audio_set_capture(true);
    xz_ui_set_state("LISTENING");
    ESP_LOGI(TAG, "talk start");
    return ESP_OK;
}

static void think_timer_start(void)
{
    s_thinking = true;
    if (s_think_timer != NULL) {
        esp_timer_stop(s_think_timer);
        esp_timer_start_once(s_think_timer, THINK_TIMEOUT_MS * 1000);
    }
}

static void think_timer_cancel(void)
{
    s_thinking = false;
    if (s_think_timer != NULL) {
        esp_timer_stop(s_think_timer);
    }
}

static void chat_stop_listening(const char *state)
{
    xz_audio_set_capture(false);
    if (!s_listening) {
        return;
    }
    s_listening = false;

    /*
     * 点得太快 / 根本没说话：本地直接收尾，不等服务端识别。
     * 否则服务端对着一段空音频做 ASR，界面会一直卡在 THINKING。
     */
    if (s_tx_frames < 2) {
        ESP_LOGW(TAG, "only %u audio frames, skip stt", (unsigned)s_tx_frames);
        if (s_channel_open) {
            esp_xiaozhi_chat_send_stop_listening(s_chat);   /* 让服务端回到空闲 */
        }
        xz_ui_set_state("IDLE");
        xz_ui_set_detail("没听到声音，再说一次");
        return;
    }

    if (s_channel_open) {
        esp_err_t err = esp_xiaozhi_chat_send_stop_listening(s_chat);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "listen stop failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "talk stop, waiting for stt (%u frames)", (unsigned)s_tx_frames);
            think_timer_start();     /* 服务端没回文本就靠它解锁 */
        }
    }
    xz_ui_set_state(state);
}

/* -------------------------------------------------------------------------- */
/* 协议事件                                                                   */

static void on_audio(const uint8_t *data, int len, void *ctx)
{
    if (data == NULL || len <= 0) {
        return;
    }

    /* 诊断：每 10 包打一次内部 RAM 余量，用来确认内存在哪一段路上被吃掉 */
    static uint32_t s_rx_count = 0;
    const uint32_t count = ++s_rx_count;
    if ((count % 10) == 1) {
        ESP_LOGI(TAG, "[heap] rx %u int=%u dma=%u psram=%u", (unsigned)count,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }

    xz_audio_play_opus(data, (size_t)len);
}

static void on_chat_event(esp_xiaozhi_chat_event_t event, void *event_data, void *ctx)
{
    switch (event) {
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SPEECH_STARTED:
        think_timer_cancel();
        s_speaking = true;
        xz_ui_set_state("SPEAKING");
        s_listening = false;
        xz_audio_set_capture(false);      /* 半双工：避免把自己的声音收进去 */
        break;

    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SPEECH_STOPPED:
        /*
         * 播报结束不自动重开麦克风：manual 模式要显式 listen start，
         * 交给下一次点按钮触发，避免服务端一直等不到 stop 而超时 goodbye。
         */
        s_speaking = false;
        s_listening = false;
        xz_audio_set_capture(false);
        xz_ui_set_state("IDLE");
        break;

    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT: {
        if (event_data == NULL) {
            break;
        }
        const esp_xiaozhi_chat_text_data_t *text = (const esp_xiaozhi_chat_text_data_t *)event_data;
        if (text->text == NULL) {
            break;
        }
        const bool from_user = (text->role == ESP_XIAOZHI_CHAT_TEXT_ROLE_USER);
        ESP_LOGI(TAG, "%s: %s", from_user ? "user" : "assistant", text->text);

        /* 文本到了就别再等超时了 */
        think_timer_cancel();
        xz_ui_add_message(from_user, text->text);
        break;
    }

    case ESP_XIAOZHI_CHAT_EVENT_CHAT_ERROR: {
        const esp_xiaozhi_chat_error_info_t *err = (const esp_xiaozhi_chat_error_info_t *)event_data;
        ESP_LOGE(TAG, "chat error 0x%x (%s)", err ? err->code : 0, err && err->source ? err->source : "?");
        xz_ui_set_state("ERROR");
        break;
    }

    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SYSTEM_CMD:
        if (event_data != NULL) {
            ESP_LOGI(TAG, "server command: %s", (const char *)event_data);
        }
        break;

    default:
        break;
    }
}

/* -------------------------------------------------------------------------- */
/* 连接层事件                                                                 */

static void on_transport_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ESP_XIAOZHI_CHAT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected to server");
        /* 连接建立前用户已经点了停止：不要再开通道 */
        if (!s_active) {
            ESP_LOGW(TAG, "session already stopped, skip channel open");
            break;
        }
        xz_ui_set_state("CONNECTED");
        if (esp_xiaozhi_chat_open_audio_channel(s_chat, NULL, NULL, 0) == ESP_OK) {
            s_channel_open = true;
            chat_start_listening();
        } else {
            ESP_LOGE(TAG, "open audio channel failed");
            xz_ui_set_state("ERROR");
        }
        break;

    case ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected from server");
        s_active = false;
        s_channel_open = false;
        think_timer_cancel();
        s_listening = false;
        s_speaking = false;
        xz_audio_set_capture(false);
        xz_ui_set_state("DISCONNECTED");
        break;

    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_OPENED:
        ESP_LOGI(TAG, "audio channel opened");
        s_channel_open = true;
        break;

    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_CLOSED:
        ESP_LOGI(TAG, "audio channel closed");
        s_channel_open = false;
        think_timer_cancel();
        s_listening = false;
        s_speaking = false;
        xz_audio_set_capture(false);
        break;

    case ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE:
        ESP_LOGI(TAG, "server said goodbye");
        xz_audio_set_capture(false);
        think_timer_cancel();
        s_listening = false;
        s_speaking = false;
        s_active = false;
        xz_ui_set_state("IDLE");
        break;

    default:
        break;
    }
}

/* -------------------------------------------------------------------------- */

esp_err_t xz_chat_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    /* 1. 向服务端拉取设备配置（首次会返回激活码），同时把 MQTT/WS 参数写入 NVS */
    esp_xiaozhi_chat_info_t info = {0};
    esp_err_t err = esp_xiaozhi_chat_get_info(&info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "get_info failed (%s), trying cached config", esp_err_to_name(err));
    } else if (info.has_activation_code && info.activation_code != NULL) {
        ESP_LOGW(TAG, "activation required, code: %s", info.activation_code);
        xz_ui_set_code(info.activation_code);
        xz_ui_set_detail("请在 xiaozhi.me 绑定该激活码");
    }

    /* free_info 之后字符串会释放，协议开关先取出来 */
    const bool has_mqtt = info.has_mqtt_config;
    const bool has_websocket = info.has_websocket_config;

    esp_xiaozhi_chat_config_t cfg = ESP_XIAOZHI_CHAT_DEFAULT_CONFIG();
    cfg.audio_type = ESP_XIAOZHI_CHAT_AUDIO_TYPE_OPUS;
    cfg.audio_callback = on_audio;
    cfg.event_callback = on_chat_event;
    cfg.has_mqtt_config = has_mqtt;
    cfg.has_websocket_config = has_websocket;

    /*
     * esp_xiaozhi_chat_start() 硬性要求 mcp_engine 非空（协议层拿它当 MCP 消息通道），
     * 即使不做设备控制也必须创建一个空 engine。owns_mcp_engine=true 交给 chat 释放。
     */
    esp_err_t mcp_err = esp_mcp_create(&s_mcp);
    if (mcp_err != ESP_OK) {
        ESP_LOGE(TAG, "mcp create failed: %s", esp_err_to_name(mcp_err));
        esp_xiaozhi_chat_free_info(&info);
        return mcp_err;
    }
    cfg.mcp_engine = s_mcp;
    cfg.owns_mcp_engine = true;

    err = esp_xiaozhi_chat_init(&cfg, &s_chat);
    esp_xiaozhi_chat_free_info(&info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "chat init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_handler_instance_register(ESP_XIAOZHI_CHAT_EVENTS, ESP_EVENT_ANY_ID,
                                              on_transport_event, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "event register failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 识别超时解脱用的单次定时器 */
    if (s_think_timer == NULL) {
        const esp_timer_create_args_t timer_args = {
            .callback = think_timeout_cb,
            .name = "think_tmo",
        };
        if (esp_timer_create(&timer_args, &s_think_timer) != ESP_OK) {
            ESP_LOGW(TAG, "think timeout timer create failed");
        }
    }

    s_ready = true;
    ESP_LOGI(TAG, "chat ready (mqtt=%d ws=%d)", (int)has_mqtt, (int)has_websocket);
    return ESP_OK;
}

esp_err_t xz_chat_start_session(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_active) {
        return ESP_OK;
    }

    xz_ui_set_state("CONNECTING");
    xz_ui_set_detail("");

    esp_err_t err = esp_xiaozhi_chat_start(s_chat);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "chat start failed: %s", esp_err_to_name(err));
        xz_ui_set_state("ERROR");
        return err;
    }
    s_active = true;
    return ESP_OK;
}

esp_err_t xz_chat_stop_session(void)
{
    if (!s_ready || !s_active) {
        return ESP_OK;
    }

    /* 先落标志：CONNECTED 回调可能正在并发执行，避免它再次打开通道 */
    s_active = false;
    think_timer_cancel();
    xz_audio_set_capture(false);
    s_listening = false;
    s_speaking = false;
    xz_audio_flush();

    /* 通道没建立时 send_stop_listening 会报 "No session"，跳过 */
    if (s_channel_open) {
        esp_xiaozhi_chat_send_stop_listening(s_chat);
        esp_xiaozhi_chat_close_audio_channel(s_chat);
        s_channel_open = false;
    }
    esp_xiaozhi_chat_stop(s_chat);

    xz_ui_set_state("IDLE");
    return ESP_OK;
}

bool xz_chat_is_active(void)
{
    return s_active;
}

bool xz_chat_is_listening(void)
{
    return s_listening;
}

bool xz_chat_is_speaking(void)
{
    return s_speaking;
}

bool xz_chat_is_thinking(void)
{
    return s_thinking;
}

void xz_chat_talk_cancel(void)
{
    if (!s_ready) {
        return;
    }
    think_timer_cancel();
    s_listening = false;
    xz_audio_set_capture(false);
    xz_ui_set_state("IDLE");
    xz_ui_set_detail("已取消");
    ESP_LOGI(TAG, "talk cancelled by user");
}

esp_err_t xz_chat_talk_start(void)
{
    if (!s_ready || !s_active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_listening) {
        return ESP_OK;
    }
    return chat_start_listening();
}

esp_err_t xz_chat_talk_stop(void)
{
    if (!s_ready || !s_listening) {
        return ESP_ERR_INVALID_STATE;
    }
    chat_stop_listening("THINKING");
    return ESP_OK;
}

void xz_chat_abort_speaking(void)
{
    if (!s_ready || !s_active) {
        return;
    }
    esp_xiaozhi_chat_send_abort_speaking(s_chat, ESP_XIAOZHI_CHAT_ABORT_SPEAKING_REASON_STOP_LISTENING);
    xz_audio_flush();
    think_timer_cancel();
    s_speaking = false;
    xz_ui_set_state("IDLE");
}

void xz_chat_send_audio(const uint8_t *data, size_t len)
{
    if (!s_active || s_chat == 0 || data == NULL || len == 0) {
        return;
    }
    s_tx_frames++;
    esp_xiaozhi_chat_send_audio_data(s_chat, (const char *)data, len);
}
