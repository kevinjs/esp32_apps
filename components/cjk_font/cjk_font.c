#include "cjk_font.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"

static const char *TAG = "cjk_font";

#define STORAGE_LABEL   "storage"
#define STORAGE_BASE    "/storage"
#define MAX_FONT_SIZES  4

/* TTF 常驻 PSRAM，只读一次，多个字号共用同一份数据 */
static uint8_t *s_ttf = NULL;
static size_t s_ttf_len = 0;

static struct {
    int px;
    const lv_font_t *font;
} s_cache[MAX_FONT_SIZES];

/* -------------------------------------------------------------------------- */

static esp_err_t ensure_storage_mounted(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = STORAGE_BASE,
        .partition_label = STORAGE_LABEL,
        .max_files = 8,
        .format_if_mount_failed = false,
    };

    const esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err == ESP_OK) {
        return ESP_OK;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return ESP_OK;      /* 已经被 image_store 挂载过了 */
    }
    return err;
}

esp_err_t cjk_font_init(void)
{
    if (s_ttf != NULL) {
        return ESP_OK;
    }

    esp_err_t err = ensure_storage_mounted();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage mount failed: %s", esp_err_to_name(err));
        return err;
    }

    char path[64];
    snprintf(path, sizeof(path), "%s/%s", STORAGE_BASE, CJK_FONT_FILE);

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        ESP_LOGW(TAG, "%s not found, Chinese falls back to bitmap/Montserrat", path);
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return ESP_FAIL;
    }
    const long size = ftell(fp);
    if (size <= 0 || size > (4 * 1024 * 1024)) {
        ESP_LOGE(TAG, "%s size looks wrong (%ld)", path, size);
        fclose(fp);
        return ESP_ERR_INVALID_SIZE;
    }
    rewind(fp);

    /* PSRAM：2 MB 放内部 RAM 会把刷屏用的 DMA 内存吃光 */
    uint8_t *buf = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGE(TAG, "no PSRAM for %ld bytes font", size);
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    const size_t got = fread(buf, 1, (size_t)size, fp);
    fclose(fp);

    if (got != (size_t)size) {
        ESP_LOGE(TAG, "short read: %u/%ld", (unsigned)got, size);
        heap_caps_free(buf);
        return ESP_FAIL;
    }

    s_ttf = buf;
    s_ttf_len = (size_t)size;
    ESP_LOGI(TAG, "font loaded: %s (%u bytes, PSRAM)", path, (unsigned)s_ttf_len);
    return ESP_OK;
}

const lv_font_t *cjk_font(int px)
{
    if (s_ttf == NULL || px <= 0) {
        return NULL;
    }

    int free_slot = -1;
    for (int i = 0; i < MAX_FONT_SIZES; i++) {
        if (s_cache[i].font != NULL) {
            if (s_cache[i].px == px) {
                return s_cache[i].font;
            }
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }

    if (free_slot < 0) {
        ESP_LOGW(TAG, "font cache full, %d px not cached", px);
        return NULL;
    }

    /* 字越小用得越杂，缓存给大一些 */
    const uint32_t glyph_cache = (px <= 20) ? 256 : 96;

    const lv_font_t *font = lv_tiny_ttf_create_data_ex(s_ttf, s_ttf_len, px,
                                                       LV_FONT_KERNING_NONE, glyph_cache);
    if (font == NULL) {
        ESP_LOGE(TAG, "tiny_ttf create %d px failed", px);
        return NULL;
    }

    s_cache[free_slot].px = px;
    s_cache[free_slot].font = font;
    ESP_LOGI(TAG, "font %d px ready (%u glyph cache)", px, (unsigned)glyph_cache);
    return font;
}

const lv_font_t *cjk_font_or(int px, const lv_font_t *fallback)
{
    const lv_font_t *font = cjk_font(px);
    return font != NULL ? font : fallback;
}

bool cjk_font_ready(void)
{
    return s_ttf != NULL;
}
