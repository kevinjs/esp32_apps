/*
 * 斜向渐变背景
 *
 * LVGL 的 bg_grad_dir 只有 LV_GRAD_DIR_VER / HOR / NONE，给不了任意角度，
 * 所以这里在 PSRAM 里生成一张 466x466 RGB565 的渐变图，作为三个页面
 * （应用列表 / 设置 / Wi-Fi）的 bg_image 铺上去。图只生成一次，全部复用。
 */
#include "ui_theme.h"

#include <math.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "selector.bg";

#define BG_W 466
#define BG_H 466

/* 渐变轴与竖直方向的夹角：0° = 完全垂直（原来那样），90° = 水平。
 * 30° 是轻微的右下倾斜，既有层次又不至于把深色区挤到一角。 */
#define BG_TILT_DEG 30.0f

static uint16_t *s_pixels = NULL;
static lv_image_dsc_t s_dsc;

static uint8_t lerp_u8(uint8_t from, uint8_t to, int t)
{
    return (uint8_t)(from + ((int)to - (int)from) * t / 255);
}

static bool build_gradient(void)
{
    s_pixels = heap_caps_malloc(BG_W * BG_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (s_pixels == NULL) {
        ESP_LOGW(TAG, "no PSRAM for gradient, falling back to solid color");
        return false;
    }

    const float rad = BG_TILT_DEG * (float)M_PI / 180.0f;
    const int32_t vx = (int32_t)(sinf(rad) * 256.0f);   /* Q8 */
    const int32_t vy = (int32_t)(cosf(rad) * 256.0f);
    const int32_t den = (BG_W - 1) * vx + (BG_H - 1) * vy;

    const uint8_t r0 = (uint8_t)((UI_COLOR_BG_TOP >> 16) & 0xFF);
    const uint8_t g0 = (uint8_t)((UI_COLOR_BG_TOP >> 8) & 0xFF);
    const uint8_t b0 = (uint8_t)(UI_COLOR_BG_TOP & 0xFF);
    const uint8_t r1 = (uint8_t)((UI_COLOR_BG_BOTTOM >> 16) & 0xFF);
    const uint8_t g1 = (uint8_t)((UI_COLOR_BG_BOTTOM >> 8) & 0xFF);
    const uint8_t b1 = (uint8_t)(UI_COLOR_BG_BOTTOM & 0xFF);

    for (int y = 0; y < BG_H; y++) {
        const int32_t row = y * vy;
        uint16_t *dst = s_pixels + (size_t)y * BG_W;
        for (int x = 0; x < BG_W; x++) {
            const int t = (int)(((x * vx + row) * 255) / den);
            const lv_color_t c = lv_color_make(
                lerp_u8(r0, r1, t),
                lerp_u8(g0, g1, t),
                lerp_u8(b0, b1, t)
            );
            dst[x] = lv_color_to_u16(c);
        }
    }

    s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.w = BG_W;
    s_dsc.header.h = BG_H;
    s_dsc.header.stride = BG_W * sizeof(uint16_t);
    s_dsc.data = (const uint8_t *)s_pixels;
    s_dsc.data_size = BG_W * BG_H * sizeof(uint16_t);

    ESP_LOGI(TAG, "gradient ready: %dx%d, tilt %.0f deg, %u bytes PSRAM",
             BG_W, BG_H, (double)BG_TILT_DEG, (unsigned)s_dsc.data_size);
    return true;
}

void ui_apply_gradient(lv_obj_t *obj)
{
    /* 先铺底色，生成失败时至少还是深色背景 */
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_COLOR_BG_TOP), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);

    if (s_pixels == NULL && !build_gradient()) {
        return;
    }

    lv_obj_set_style_bg_image_src(obj, &s_dsc, 0);
    lv_obj_set_style_bg_image_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_image_tiled(obj, false, 0);
}
