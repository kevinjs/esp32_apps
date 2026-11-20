#include "app_settings.h"

#include <stdbool.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "app_settings";

#define NVS_NAMESPACE   "app_cfg"
#define KEY_SPK_VOLUME  "spk_vol"

static nvs_handle_t s_nvs = 0;
static bool s_ready = false;
static int s_volume = APP_SETTINGS_DEFAULT_VOLUME;

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
    ESP_LOGI(TAG, "speaker volume %d%%", s_volume);
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
