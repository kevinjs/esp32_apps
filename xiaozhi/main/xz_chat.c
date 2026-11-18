#include "xz_chat.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_xiaozhi_chat.h"
#include "esp_xiaozhi_info.h"

#include "xz_audio.h"
#include "xz_ui.h"

static const char *TAG = "xz.chat";

static esp_xiaozhi_chat_handle_t s_chat = 0;
static volatile bool s_ready = false;
static volatile bool s_active = false;

/* -------------------------------------------------------------------------- */
/* 协议事件                                                                   */

static void on_audio(const uint8_t *data, int len, void *ctx)
{
    if (data == NULL || len <= 0) {
        return;
    }
    xz_audio_play_opus(data, (size_t)len);
}

static void on_chat_event(esp_xiaozhi_chat_event_t event, void *event_data, void *ctx)
{
    switch (event) {
    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SPEECH_STARTED:
        xz_ui_set_state("SPEAKING");
        xz_audio_set_capture(false);      /* 半双工：避免把自己的声音收进去 */
        break;

    case ESP_XIAOZHI_CHAT_EVENT_CHAT_SPEECH_STOPPED:
        xz_ui_set_state("LISTENING");
        xz_audio_set_capture(true);
        break;

    case ESP_XIAOZHI_CHAT_EVENT_CHAT_TEXT: {
        if (event_data == NULL) {
            break;
        }
        const esp_xiaozhi_chat_text_data_t *text = (const esp_xiaozhi_chat_text_data_t *)event_data;
        if (text->text == NULL) {
            break;
        }
        ESP_LOGI(TAG, "%s: %s", text->role == ESP_XIAOZHI_CHAT_TEXT_ROLE_USER ? "user" : "assistant",
                 text->text);
        xz_ui_set_detail(text->text);
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
        xz_ui_set_state("CONNECTED");
        if (esp_xiaozhi_chat_open_audio_channel(s_chat, NULL, NULL, 0) == ESP_OK) {
            esp_xiaozhi_chat_send_start_listening(s_chat, ESP_XIAOZHI_CHAT_LISTENING_MODE_MANUAL);
            xz_audio_set_capture(true);
            xz_ui_set_state("LISTENING");
        } else {
            ESP_LOGE(TAG, "open audio channel failed");
            xz_ui_set_state("ERROR");
        }
        break;

    case ESP_XIAOZHI_CHAT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected from server");
        s_active = false;
        xz_audio_set_capture(false);
        xz_ui_set_state("DISCONNECTED");
        break;

    case ESP_XIAOZHI_CHAT_EVENT_AUDIO_CHANNEL_CLOSED:
        ESP_LOGI(TAG, "audio channel closed");
        xz_audio_set_capture(false);
        break;

    case ESP_XIAOZHI_CHAT_EVENT_SERVER_GOODBYE:
        ESP_LOGI(TAG, "server said goodbye");
        xz_audio_set_capture(false);
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
        xz_ui_set_detail("bind this code at xiaozhi.me");
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
    cfg.mcp_engine = NULL;          /* 第一版不做设备端 MCP 控制 */
    cfg.owns_mcp_engine = false;

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

    xz_audio_set_capture(false);
    xz_audio_flush();
    esp_xiaozhi_chat_send_stop_listening(s_chat);
    esp_xiaozhi_chat_close_audio_channel(s_chat);
    esp_xiaozhi_chat_stop(s_chat);

    s_active = false;
    xz_ui_set_state("IDLE");
    return ESP_OK;
}

bool xz_chat_is_active(void)
{
    return s_active;
}

void xz_chat_send_audio(const uint8_t *data, size_t len)
{
    if (!s_active || s_chat == 0 || data == NULL || len == 0) {
        return;
    }
    esp_xiaozhi_chat_send_audio_data(s_chat, (const char *)data, len);
}
