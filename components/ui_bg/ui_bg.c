#include "ui_bg.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "ui.bg";

#define BG_SIZE     UI_BG_SIZE
#define DEG2RAD     0.017453292519943295f

static uint16_t *s_pixels = NULL;
static lv_image_dsc_t s_dsc;
static bool s_failed = false;       /* 生成失败只试一次，避免每帧都去申请 PSRAM */

static uint8_t lerp_u8(uint8_t from, uint8_t to, int t)
{
    return (uint8_t)(from + ((int)to - (int)from) * t / 255);
}

static bool build_gradient(void)
{
    if (s_pixels != NULL) {
        return true;
    }
    if (s_failed) {
        return false;
    }

    s_pixels = heap_caps_malloc(BG_SIZE * BG_SIZE * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (s_pixels == NULL) {
        ESP_LOGW(TAG, "no PSRAM for gradient, falling back to solid color");
        s_failed = true;
        return false;
    }

    /* 渐变轴：0° = 竖直向下，30° = 略微右下倾斜（像 selector 那样从左上暗到右下亮） */
    const float rad = UI_BG_TILT_DEG * DEG2RAD;
    const int32_t vx = (int32_t)(sinf(rad) * 256.0f);   /* Q8 */
    const int32_t vy = (int32_t)(cosf(rad) * 256.0f);
    const int32_t den = (BG_SIZE - 1) * vx + (BG_SIZE - 1) * vy;

    const uint8_t r0 = (uint8_t)((UI_BG_COLOR_TOP >> 16) & 0xFF);
    const uint8_t g0 = (uint8_t)((UI_BG_COLOR_TOP >> 8) & 0xFF);
    const uint8_t b0 = (uint8_t)(UI_BG_COLOR_TOP & 0xFF);
    const uint8_t r1 = (uint8_t)((UI_BG_COLOR_BOTTOM >> 16) & 0xFF);
    const uint8_t g1 = (uint8_t)((UI_BG_COLOR_BOTTOM >> 8) & 0xFF);
    const uint8_t b1 = (uint8_t)(UI_BG_COLOR_BOTTOM & 0xFF);

    for (int y = 0; y < BG_SIZE; y++) {
        const int32_t row = y * vy;
        uint16_t *dst = s_pixels + (size_t)y * BG_SIZE;
        for (int x = 0; x < BG_SIZE; x++) {
            const int t = (int)(((x * vx + row) * 255) / den);
            const lv_color_t c = lv_color_make(lerp_u8(r0, r1, t),
                                               lerp_u8(g0, g1, t),
                                               lerp_u8(b0, b1, t));
            dst[x] = lv_color_to_u16(c);
        }
    }

    s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.w = BG_SIZE;
    s_dsc.header.h = BG_SIZE;
    s_dsc.header.stride = BG_SIZE * sizeof(uint16_t);
    s_dsc.data = (const uint8_t *)s_pixels;
    s_dsc.data_size = BG_SIZE * BG_SIZE * sizeof(uint16_t);

    ESP_LOGI(TAG, "gradient ready: %dx%d, tilt %.0f deg, %u bytes PSRAM",
             BG_SIZE, BG_SIZE, (double)UI_BG_TILT_DEG, (unsigned)s_dsc.data_size);
    return true;
}

void ui_bg_apply(lv_obj_t *obj)
{
    if (obj == NULL) {
        return;
    }

    /* 先铺底色：生成失败时至少还是深色背景，不是白屏 */
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_BG_COLOR_TOP), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);

    if (!build_gradient()) {
        return;
    }

    lv_obj_set_style_bg_image_src(obj, &s_dsc, 0);
    lv_obj_set_style_bg_image_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_image_tiled(obj, false, 0);
}
