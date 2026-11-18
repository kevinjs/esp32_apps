#include "boot_switch.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "boot_switch";

#define NVS_NAMESPACE   "boot_switch"
#define NVS_KEY_TRIES   "tries"
#define KEY_POLL_MS     50

static boot_switch_config_t s_config = BOOT_SWITCH_CONFIG_DEFAULT();
static char s_running_slot[17] = "unknown";
static bool s_running_from_selector = false;
static bool s_initialized = false;

/* -------------------------------------------------------------------------- */
/*  NVS                                                                       */
/* -------------------------------------------------------------------------- */

static esp_err_t open_nvs(nvs_handle_t *out)
{
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, out);
    if (err != ESP_ERR_NVS_NOT_INITIALIZED) {
        return err;
    }

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* 这里刻意不擦除：nvs 里存着 IMU 零偏和配网信息，宁可本次不计数。 */
        ESP_LOGE(TAG, "NVS needs reinit (%s); not erasing to keep calibration", esp_err_to_name(err));
        return err;
    }
    if (err != ESP_OK) {
        return err;
    }
    return nvs_open(NVS_NAMESPACE, NVS_READWRITE, out);
}

static uint8_t read_tries(void)
{
    nvs_handle_t handle;
    uint8_t tries = 0;
    if (open_nvs(&handle) != ESP_OK) {
        return 0;
    }
    (void)nvs_get_u8(handle, NVS_KEY_TRIES, &tries);
    nvs_close(handle);
    return tries;
}

static void write_tries(uint8_t tries)
{
    nvs_handle_t handle;
    if (open_nvs(&handle) != ESP_OK) {
        return;
    }
    (void)nvs_set_u8(handle, NVS_KEY_TRIES, tries);
    (void)nvs_commit(handle);
    nvs_close(handle);
}

/* -------------------------------------------------------------------------- */
/*  启动槽                                                                     */
/* -------------------------------------------------------------------------- */

static void resolve_running_partition(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        ESP_LOGW(TAG, "cannot determine running partition");
        return;
    }
    strncpy(s_running_slot, running->label, sizeof(s_running_slot) - 1);
    s_running_slot[sizeof(s_running_slot) - 1] = '\0';
    s_running_from_selector = (running->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY);
}

/* -------------------------------------------------------------------------- */
/*  按键兜底                                                                   */
/* -------------------------------------------------------------------------- */

static void boot_key_task(void *arg)
{
    (void)arg;

    bool holding = false;
    bool triggered = false;
    int64_t hold_start_us = 0;

    while (true) {
        const int level = gpio_get_level((gpio_num_t)s_config.boot_key_gpio);

        if (level == 0) {
            const int64_t now_us = esp_timer_get_time();
            if (!holding) {
                holding = true;
                triggered = false;
                hold_start_us = now_us;
            } else if (!triggered &&
                       (now_us - hold_start_us) / 1000 >= (int64_t)s_config.boot_key_hold_ms) {
                triggered = true;
                ESP_LOGW(TAG, "BOOT(GPIO%d) held %u ms, returning to selector",
                         s_config.boot_key_gpio, (unsigned)s_config.boot_key_hold_ms);
                esp_err_t err = boot_switch_request_selector();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "return to selector failed: %s", esp_err_to_name(err));
                }
            }
        } else {
            holding = false;
            triggered = false;
        }

        vTaskDelay(pdMS_TO_TICKS(KEY_POLL_MS));
    }
}

static void start_boot_key_watch(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << s_config.boot_key_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BOOT GPIO config failed: %s", esp_err_to_name(err));
        return;
    }

    BaseType_t created = xTaskCreate(boot_key_task, "boot_switch", 3072, NULL, 3, NULL);
    if (created != pdPASS) {
        ESP_LOGW(TAG, "BOOT key task create failed");
    }
}

/* -------------------------------------------------------------------------- */
/*  对外接口                                                                   */
/* -------------------------------------------------------------------------- */

esp_err_t boot_switch_init(const boot_switch_config_t *config)
{
    if (config != NULL) {
        s_config = *config;
    }
    if (s_config.boot_key_hold_ms == 0) {
        s_config.boot_key_hold_ms = BOOT_SWITCH_KEY_HOLD_MS_DEFAULT;
    }

    resolve_running_partition();
    s_initialized = true;
    ESP_LOGI(TAG, "running partition: %s%s", s_running_slot,
             s_running_from_selector ? " (selector)" : "");

    if (!s_running_from_selector && s_config.max_boot_attempts > 0) {
        const uint8_t tries = read_tries();
        if (tries >= s_config.max_boot_attempts) {
            ESP_LOGE(TAG, "%u failed boots in a row, falling back to selector", tries);
            write_tries(0);
            esp_err_t err = boot_switch_request_selector();
            /* 正常情况下不会返回 */
            ESP_LOGE(TAG, "fallback failed: %s", esp_err_to_name(err));
            return err;
        }
        write_tries((uint8_t)(tries + 1));
        ESP_LOGI(TAG, "boot attempt %u/%u", (unsigned)(tries + 1),
                 (unsigned)s_config.max_boot_attempts);
    }

    if (s_config.watch_boot_key && !s_running_from_selector) {
        start_boot_key_watch();
    }

    return ESP_OK;
}

esp_err_t boot_switch_mark_boot_ok(void)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_running_from_selector) {
        return ESP_OK;
    }
    write_tries(0);
    return ESP_OK;
}

esp_err_t boot_switch_request_selector(void)
{
    if (s_running_from_selector) {
        ESP_LOGW(TAG, "already in selector, ignoring switch request");
        return ESP_ERR_INVALID_STATE;
    }

    const esp_partition_t *selector = esp_partition_find_first(
                                          ESP_PARTITION_TYPE_APP,
                                          ESP_PARTITION_SUBTYPE_APP_FACTORY,
                                          NULL
                                      );
    if (selector == NULL) {
        ESP_LOGE(TAG, "no factory partition, cannot return to selector");
        return ESP_ERR_NOT_FOUND;
    }

    /*
     * 擦掉 otadata 让 bootloader 回落到 factory，比 esp_ota_set_boot_partition(factory)
     * 更可控：不依赖 IDF 对 factory 子类型的特殊处理。
     */
    const esp_partition_t *otadata = esp_partition_find_first(
                                         ESP_PARTITION_TYPE_DATA,
                                         ESP_PARTITION_SUBTYPE_DATA_OTA,
                                         NULL
                                     );
    if (otadata == NULL) {
        ESP_LOGE(TAG, "no otadata partition, cannot switch boot slot");
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = esp_partition_erase_range(otadata, 0, otadata->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erase otadata failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "rebooting into selector");
    esp_restart();
    return ESP_OK; /* 不会执行到这里 */
}

esp_err_t boot_switch_set_boot_slot(const char *label)
{
    if (label == NULL || label[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *target = esp_partition_find_first(
                                        ESP_PARTITION_TYPE_APP,
                                        ESP_PARTITION_SUBTYPE_ANY,
                                        label
                                    );
    if (target == NULL) {
        ESP_LOGE(TAG, "app partition not found: %s", label);
        return ESP_ERR_NOT_FOUND;
    }
    if (target->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY) {
        ESP_LOGE(TAG, "use boot_switch_request_selector() for factory");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set boot partition %s failed: %s", label, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "next boot slot: %s", label);
    return ESP_OK;
}

const char *boot_switch_running_slot(void)
{
    return s_running_slot;
}

bool boot_switch_running_from_selector(void)
{
    return s_running_from_selector;
}
