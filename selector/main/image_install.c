#include "image_install.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "selector.install";

#define STORAGE_BASE        "/storage"
#define RUN_SLOT_LABEL      "ota_0"
#define CHUNK_SIZE          4096
#define TASK_STACK_SIZE     6144
#define TASK_PRIORITY       5

static SemaphoreHandle_t s_lock = NULL;
static image_install_status_t s_status = {
    .state = IMAGE_INSTALL_IDLE,
    .percent = 0,
    .message = "",
};

static void status_update(image_install_state_t state, int percent, const char *message)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    s_status.state = state;
    s_status.percent = percent;
    if (message != NULL) {
        strncpy(s_status.message, message, sizeof(s_status.message) - 1);
        s_status.message[sizeof(s_status.message) - 1] = '\0';
    }
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

static bool ensure_lock(void)
{
    if (s_lock != NULL) {
        return true;
    }
    s_lock = xSemaphoreCreateMutex();
    return s_lock != NULL;
}

/** @brief 安装工作函数，返回后由任务负责删除自己。 */
static void install_worker(const char *file)
{
    const esp_partition_t *slot = esp_partition_find_first(
                                      ESP_PARTITION_TYPE_APP,
                                      ESP_PARTITION_SUBTYPE_ANY,
                                      RUN_SLOT_LABEL
                                  );
    if (slot == NULL) {
        status_update(IMAGE_INSTALL_FAILED, 0, "run slot ota_0 not found");
        return;
    }

    char path[64];
    snprintf(path, sizeof(path), "%s/%s", STORAGE_BASE, file);

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        status_update(IMAGE_INSTALL_FAILED, 0, "cannot open image file");
        return;
    }

    fseek(fp, 0, SEEK_END);
    const long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (file_size <= 0) {
        fclose(fp);
        status_update(IMAGE_INSTALL_FAILED, 0, "image file is empty");
        return;
    }
    if ((size_t)file_size > slot->size) {
        ESP_LOGE(TAG, "image %ld bytes exceeds run slot %u bytes", file_size, (unsigned)slot->size);
        fclose(fp);
        status_update(IMAGE_INSTALL_FAILED, 0, "image larger than run slot, resize partition table");
        return;
    }

    uint8_t *buffer = malloc(CHUNK_SIZE);
    if (buffer == NULL) {
        fclose(fp);
        status_update(IMAGE_INSTALL_FAILED, 0, "out of memory");
        return;
    }

    status_update(IMAGE_INSTALL_RUNNING, 0, "writing");

    esp_ota_handle_t handle = 0;
    bool begun = false;
    esp_err_t err = esp_ota_begin(slot, (size_t)file_size, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        free(buffer);
        fclose(fp);
        status_update(IMAGE_INSTALL_FAILED, 0, "cannot erase/write run slot");
        return;
    }
    begun = true;

    size_t written = 0;
    int last_percent = -1;
    while (written < (size_t)file_size) {
        const size_t want = ((size_t)file_size - written) < CHUNK_SIZE
                            ? ((size_t)file_size - written) : CHUNK_SIZE;
        const size_t got = fread(buffer, 1, want, fp);
        if (got == 0) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }

        err = esp_ota_write(handle, buffer, got);
        if (err != ESP_OK) {
            break;
        }

        written += got;
        const int percent = (int)((uint64_t)written * 100 / (uint64_t)file_size);
        if (percent != last_percent) {
            last_percent = percent;
            status_update(IMAGE_INSTALL_RUNNING, percent, "writing");
        }
    }

    free(buffer);
    fclose(fp);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write failed: %s", esp_err_to_name(err));
        if (begun) {
            esp_ota_abort(handle);
        }
        status_update(IMAGE_INSTALL_FAILED, 0, "write failed, image may be corrupted");
        return;
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        status_update(IMAGE_INSTALL_FAILED, 0, "image verification failed");
        return;
    }

    err = esp_ota_set_boot_partition(slot);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set boot partition failed: %s", esp_err_to_name(err));
        status_update(IMAGE_INSTALL_FAILED, 0, "failed to switch boot partition");
        return;
    }

    ESP_LOGI(TAG, "wrote %s (%ld bytes), switching boot to %s", file, file_size, RUN_SLOT_LABEL);
    status_update(IMAGE_INSTALL_DONE, 100, "install done, rebooting");
}

static void install_task(void *arg)
{
    char *file = (char *)arg;
    install_worker(file);
    free(file);
    vTaskDelete(NULL);
}

esp_err_t image_install_start(const char *file)
{
    if (file == NULL || file[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ensure_lock()) {
        return ESP_ERR_NO_MEM;
    }

    image_install_status_t current;
    image_install_status(&current);
    if (current.state == IMAGE_INSTALL_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }

    char *file_copy = strdup(file);
    if (file_copy == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* 先置状态，避免任务还没跑起来时 UI 又触发一次 */
    status_update(IMAGE_INSTALL_RUNNING, 0, "preparing install");

    BaseType_t created = xTaskCreate(install_task, "app_install",
                                     TASK_STACK_SIZE, file_copy, TASK_PRIORITY, NULL);
    if (created != pdPASS) {
        free(file_copy);
        status_update(IMAGE_INSTALL_FAILED, 0, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void image_install_status(image_install_status_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    *out = s_status;
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

void image_install_reset(void)
{
    status_update(IMAGE_INSTALL_IDLE, 0, "");
}
