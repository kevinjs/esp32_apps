#include "app_settings.h"

#include <stdbool.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "app_settings";

#define NVS_NAMESPACE   "app_cfg"
#define KEY_SPK_VOLUME  "spk_vol"
#define KEY_BRIGHTNESS  "bright"

static nvs_handle_t s_nvs = 0;
static bool s_ready = false;
static int s_volume = APP_SETTINGS_DEFAULT_VOLUME;
static int s_brightness = APP_SETTINGS_DEFAULT_BRIGHTNESS;

static int clamp_volume(int volume)
{
    if (volume < 0) {
        return 0;
    }
    if (volume > 100) {
        return 100;
    }
    return volume;
}

/* 下限 10：调到 0 会黑屏，之后靠手指摸设置太难了 */
static int clamp_brightness(int percent)
{
    if (percent < 10) {
        return 10;
    }
    if (percent > 100) {
        return 100;
    }
    return percent;
}

esp_err_t app_settings_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s, using defaults", esp_err_to_name(err));
        return err;
    }
    s_ready = true;

    int32_t volume = APP_SETTINGS_DEFAULT_VOLUME;
    if (nvs_get_i32(s_nvs, KEY_SPK_VOLUME, &volume) == ESP_OK) {
        s_volume = clamp_volume((int)volume);
    }

    int32_t brightness = APP_SETTINGS_DEFAULT_BRIGHTNESS;
    if (nvs_get_i32(s_nvs, KEY_BRIGHTNESS, &brightness) == ESP_OK) {
        s_brightness = clamp_brightness((int)brightness);
    }

    ESP_LOGI(TAG, "speaker volume %d%%, brightness %d%%", s_volume, s_brightness);
    return ESP_OK;
}

int app_settings_get_speaker_volume(void)
{
    if (!s_ready) {
        app_settings_init();
    }
    return s_volume;
}

esp_err_t app_settings_set_speaker_volume(int volume)
{
    s_volume = clamp_volume(volume);

    if (!s_ready && app_settings_init() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = nvs_set_i32(s_nvs, KEY_SPK_VOLUME, s_volume);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save volume failed: %s", esp_err_to_name(err));
    }
    return err;
}

int app_settings_get_brightness(void)
{
    if (!s_ready) {
        app_settings_init();
    }
    return s_brightness;
}

esp_err_t app_settings_set_brightness(int percent)
{
    s_brightness = clamp_brightness(percent);

    if (!s_ready && app_settings_init() != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = nvs_set_i32(s_nvs, KEY_BRIGHTNESS, s_brightness);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save brightness failed: %s", esp_err_to_name(err));
    }
    return err;
}
