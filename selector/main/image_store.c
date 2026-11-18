#include "image_store.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_spiffs.h"

static const char *TAG = "selector.store";

#define STORAGE_BASE        "/storage"
#define STORAGE_LABEL       "storage"
#define APP_IMAGE_MAGIC     0xE9
/* esp_image_header_t(24) + esp_image_segment_header_t(8) 后紧跟 esp_app_desc_t */
#define APP_DESC_OFFSET     0x20

static bool ends_with(const char *text, const char *suffix)
{
    const size_t text_len = strlen(text);
    const size_t suffix_len = strlen(suffix);
    if (suffix_len > text_len) {
        return false;
    }
    return strcmp(text + text_len - suffix_len, suffix) == 0;
}

esp_err_t image_store_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = STORAGE_BASE,
        .partition_label = STORAGE_LABEL,
        .max_files = 8,
        .format_if_mount_failed = false,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err == ESP_OK) {
        return ESP_OK;
    }

    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "no storage partition in partition table");
        return err;
    }

    /* 首次烧录 / 镜像被破坏：格式化后镜像库为空，用户需要重新 pack_storage 一次 */
    ESP_LOGW(TAG, "mount storage failed (%s), trying format", esp_err_to_name(err));
    esp_err_t fmt_err = esp_spiffs_format(STORAGE_LABEL);
    if (fmt_err != ESP_OK) {
        ESP_LOGE(TAG, "format storage failed: %s", esp_err_to_name(fmt_err));
        return fmt_err;
    }
    err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount still failed after format: %s", esp_err_to_name(err));
    }
    return err;
}

/** @brief 读取文件头，校验镜像格式并取出 app 描述符。 */
static esp_err_t load_descriptor(const char *file, image_entry_t *entry)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", STORAGE_BASE, file);

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t header[APP_DESC_OFFSET + sizeof(esp_app_desc_t)];
    const size_t read_len = fread(header, 1, sizeof(header), fp);
    if (read_len != sizeof(header) || header[0] != APP_IMAGE_MAGIC) {
        fclose(fp);
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_app_desc_t desc;
    memcpy(&desc, header + APP_DESC_OFFSET, sizeof(desc));
    if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        fclose(fp);
        return ESP_ERR_INVALID_RESPONSE;
    }

    fseek(fp, 0, SEEK_END);
    const long file_size = ftell(fp);
    fclose(fp);
    if (file_size <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    snprintf(entry->file, sizeof(entry->file), "%s", file);
    snprintf(entry->name, sizeof(entry->name), "%.*s",
             (int)sizeof(desc.project_name), desc.project_name);
    snprintf(entry->version, sizeof(entry->version), "%.*s",
             (int)sizeof(desc.version), desc.version);
    entry->size = (uint32_t)file_size;
    return ESP_OK;
}

static int compare_entries(const void *lhs, const void *rhs)
{
    const image_entry_t *a = (const image_entry_t *)lhs;
    const image_entry_t *b = (const image_entry_t *)rhs;
    return strcmp(a->file, b->file);
}

int image_store_scan(image_entry_t *out, int max_count)
{
    if (out == NULL || max_count <= 0) {
        return 0;
    }

    DIR *dir = opendir(STORAGE_BASE);
    if (dir == NULL) {
        ESP_LOGW(TAG, "cannot open %s", STORAGE_BASE);
        return 0;
    }

    int count = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL && count < max_count) {
        if (!ends_with(entry->d_name, ".bin")) {
            continue;
        }
        image_entry_t parsed = {0};
        esp_err_t err = load_descriptor(entry->d_name, &parsed);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "skipping invalid image %s (%s)", entry->d_name, esp_err_to_name(err));
            continue;
        }
        out[count++] = parsed;
    }
    closedir(dir);

    qsort(out, count, sizeof(out[0]), compare_entries);
    ESP_LOGI(TAG, "image library: %d usable images", count);
    return count;
}

void image_store_info(size_t *total_bytes, size_t *used_bytes)
{
    if (total_bytes != NULL) {
        *total_bytes = 0;
    }
    if (used_bytes != NULL) {
        *used_bytes = 0;
    }

    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info(STORAGE_LABEL, &total, &used) == ESP_OK) {
        if (total_bytes != NULL) {
            *total_bytes = total;
        }
        if (used_bytes != NULL) {
            *used_bytes = used;
        }
    }
}
