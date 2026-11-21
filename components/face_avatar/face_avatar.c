#include "face_avatar.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"

static const char *TAG = "face_avatar";

#define STORAGE_LABEL   "storage"
#define STORAGE_BASE    "/storage"
#define FACE_MAX_BYTES  (1024 * 1024)   /* 单张上限：192² × 4 ≈ 148 KB，留足余量 */
#define TALK_MAX        8

/* 名字必须与 storage_src/faces/face-<name>.png 一致（全小写、短横线） */
static const char *const s_names[FACE_AVATAR_MOOD_NUM] = {
    [FACE_AVATAR_WAIT]   = "wait",
    [FACE_AVATAR_IDLE]   = "idle",
    [FACE_AVATAR_BLINK]  = "blink",
    [FACE_AVATAR_LISTEN] = "listen",
    [FACE_AVATAR_THINK]  = "think",
    [FACE_AVATAR_SAD]    = "sad",
    [FACE_AVATAR_HAPPY]  = "happy",
};

typedef struct {
    lv_image_dsc_t dsc;
    uint8_t *raw;       /* header + 像素整块，PSRAM */
} face_slot_t;

static face_slot_t s_cache[FACE_AVATAR_MOOD_NUM];
static face_slot_t s_talk[TALK_MAX];
static int s_talk_count = -1;       /* -1 = 尚未扫描 */
static bool s_mounted = false;
static bool s_ready = false;
static uint16_t s_size = 0;

/* -------------------------------------------------------------------------- */

static esp_err_t ensure_storage_mounted(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = STORAGE_BASE,
        .partition_label = STORAGE_LABEL,
        .max_files = 8,
        .format_if_mount_failed = false,
    };

    const esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        s_mounted = true;       /* INVALID_STATE = 已被别的组件挂载 */
        return ESP_OK;
    }
    return err;
}

static void slot_free(face_slot_t *slot)
{
    if (slot->raw != NULL) {
        heap_caps_free(slot->raw);
        slot->raw = NULL;
    }
    memset(&slot->dsc, 0, sizeof(slot->dsc));
}

/**
 * @brief 读一个表情 bin 到 PSRAM 并填好 lv_image_dsc_t。
 *
 * 校验偏严：header 里的 magic/格式/尺寸/stride 与文件长度必须自洽，
 * 免得打包脚本换了格式之后固件按错的内存布局去画（花屏/越界）。
 */
static bool load_one(const char *name, face_slot_t *slot)
{
    if (slot->raw != NULL) {
        return true;
    }
    if (name == NULL || slot->raw != NULL) {
        return false;
    }

    char path[64];
    snprintf(path, sizeof(path), "%s/face-%s.bin", STORAGE_BASE, name);

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return false;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    const long size = ftell(fp);
    if (size <= (long)sizeof(lv_image_header_t) || size > FACE_MAX_BYTES) {
        ESP_LOGW(TAG, "%s: bad size %ld", path, size);
        fclose(fp);
        return false;
    }
    rewind(fp);

    /* PSRAM：一张 192² 要 148 KB，内部 RAM 放不下也不该放 */
    uint8_t *buf = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        ESP_LOGE(TAG, "no PSRAM for %s (%ld bytes)", path, size);
        fclose(fp);
        return false;
    }

    const size_t got = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    if (got != (size_t)size) {
        ESP_LOGE(TAG, "%s: short read %u/%ld", path, (unsigned)got, size);
        heap_caps_free(buf);
        return false;
    }

    const lv_image_header_t *hdr = (const lv_image_header_t *)buf;
    const size_t pixels = (size_t)size - sizeof(lv_image_header_t);

    if (hdr->magic != LV_IMAGE_HEADER_MAGIC ||
            hdr->cf != LV_COLOR_FORMAT_ARGB8888 ||
            hdr->w != hdr->h || hdr->w < 8 || hdr->w > 512 ||
            hdr->stride != hdr->w * 4 ||
            pixels != (size_t)hdr->stride * hdr->h) {
        ESP_LOGE(TAG, "%s: unexpected layout (magic=%02x cf=%02x %ux%u stride=%u, %u bytes)",
                 path, (unsigned)hdr->magic, (unsigned)hdr->cf,
                 (unsigned)hdr->w, (unsigned)hdr->h, (unsigned)hdr->stride, (unsigned)pixels);
        heap_caps_free(buf);
        return false;
    }

    slot->raw = buf;
    slot->dsc.header = *hdr;
    slot->dsc.data_size = (uint32_t)pixels;
    slot->dsc.data = buf + sizeof(lv_image_header_t);

    if (s_size == 0) {
        s_size = (uint16_t)hdr->w;
    }
    return true;
}

/* -------------------------------------------------------------------------- */

esp_err_t face_avatar_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    const esp_err_t err = ensure_storage_mounted();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage mount failed: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * idle 是必备素材：它拿不到就说明表情包没打进镜像库，
     * 让调用方回退到自绘表情，而不是画一颗半透明的空图。
     */
    if (!load_one(s_names[FACE_AVATAR_IDLE], &s_cache[FACE_AVATAR_IDLE])) {
        ESP_LOGW(TAG, "%s/face-idle.bin not found, use built-in face", STORAGE_BASE);
        return ESP_ERR_NOT_FOUND;
    }

    s_ready = true;
    ESP_LOGI(TAG, "avatar ready: %ux%u (idle %u bytes, PSRAM)", (unsigned)s_size,
             (unsigned)s_size, (unsigned)s_cache[FACE_AVATAR_IDLE].dsc.data_size);

    /* 其余素材按需加载，缺失只是没有对应表情，不影响启动 */
    return ESP_OK;
}

bool face_avatar_ready(void)
{
    return s_ready;
}

uint16_t face_avatar_size(void)
{
    return s_size;
}

const lv_image_dsc_t *face_avatar_get(face_avatar_mood_t mood)
{
    if (!s_ready || mood < 0 || mood >= FACE_AVATAR_MOOD_NUM) {
        return NULL;
    }
    face_slot_t *slot = &s_cache[mood];
    if (!load_one(s_names[mood], slot)) {
        return NULL;
    }
    return &slot->dsc;
}

const lv_image_dsc_t *face_avatar_by_name(const char *name)
{
    if (!s_ready || name == NULL) {
        return NULL;
    }
    for (int i = 0; i < FACE_AVATAR_MOOD_NUM; i++) {
        /* FACE_AVATAR_TALK 没有单帧文件，走 face_avatar_talk_frame() */
        if (s_names[i] != NULL && strcmp(s_names[i], name) == 0) {
            return face_avatar_get((face_avatar_mood_t)i);
        }
    }
    return NULL;
}

static void scan_talk(void)
{
    if (s_talk_count >= 0) {
        return;
    }

    s_talk_count = 0;
    for (int i = 0; i < TALK_MAX; i++) {
        char name[16];
        snprintf(name, sizeof(name), "talk-%d", i + 1);
        if (!load_one(name, &s_talk[i])) {
            break;
        }
        s_talk_count++;
    }

    if (s_talk_count > 0) {
        ESP_LOGI(TAG, "talk frames: %d", s_talk_count);
    }
}

int face_avatar_talk_frames(void)
{
    if (!s_ready) {
        return 0;
    }
    scan_talk();
    return s_talk_count;
}

const lv_image_dsc_t *face_avatar_talk_frame(int index)
{
    if (!s_ready) {
        return NULL;
    }
    scan_talk();
    if (index < 0 || index >= s_talk_count) {
        return NULL;
    }
    return &s_talk[index].dsc;
}

void face_avatar_release(void)
{
    for (int i = 0; i < FACE_AVATAR_MOOD_NUM; i++) {
        slot_free(&s_cache[i]);
    }
    for (int i = 0; i < TALK_MAX; i++) {
        slot_free(&s_talk[i]);
    }
    s_talk_count = -1;
    s_ready = false;
    s_size = 0;
}
