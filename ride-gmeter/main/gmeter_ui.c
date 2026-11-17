#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "lvgl.h"
#include "bsp/esp-bsp.h"

#include "gmeter_config.h"
#include "gmeter_ui.h"

static const char *TAG = "gmeter.ui";

/* 屏幕与布局 */
#define UI_SIZE         466
#define UI_HALF         (UI_SIZE / 2)
#define BUBBLE_R        13
#define MOVE_R          140     /* 气泡最大偏移（对应满量程） */

/* 底部圆弧条：以屏幕中心为圆心，下半圆左右各 90° 为满量程 */
#define ARC_SIZE        410
#define ARC_W           12
#define ARC_BOTTOM_DEG  90      /* 6 点钟方向 */
#define ARC_HALF_SPAN   90      /* 单侧满量程跨过的角度 */

/* 颜色 */
#define COL_BG          0x000000
#define COL_RING        0x1E1E1E
#define COL_CROSS       0x2A2A2A
#define COL_IDLE        0x555555
#define COL_OK          0x2ECC71
#define COL_WARN        0xF1C40F
#define COL_DANGER      0xE74C3C

static lv_obj_t *s_hint;
static lv_obj_t *s_bubble;
static lv_obj_t *s_val;
static lv_obj_t *s_unit;
static lv_obj_t *s_peak;
static lv_obj_t *s_state;
static lv_obj_t *s_arc_bg;
static lv_obj_t *s_arc_accel;
static lv_obj_t *s_arc_brake;
static lv_obj_t *s_road;
static lv_obj_t *s_turn;

static float s_peak_mag;
static float s_bx, s_by;      /* 平滑后的气泡偏移（px） */
static float s_vert_avg;      /* 垂直方向加速度的平滑值（路面强度） */
static int32_t s_red_ms;      /* 高加速度已持续的时间（ms） */

static gmeter_ui_data_t s_snap;
static portMUX_TYPE s_snap_mux = portMUX_INITIALIZER_UNLOCKED;

/* -------------------------------------------------------------------------- */

void gmeter_ui_post(const gmeter_ui_data_t *data)
{
    if (data == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_snap_mux);
    s_snap = *data;
    taskEXIT_CRITICAL(&s_snap_mux);
}

void gmeter_ui_set_hint(const char *text)
{
    if (s_hint == NULL || text == NULL) {
        return;
    }
    if (bsp_display_lock(200) != ESP_OK) {
        return;
    }
    lv_label_set_text(s_hint, text);
    bsp_display_unlock();
}

static lv_obj_t *make_obj(lv_obj_t *parent, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_scroll_dir(obj, LV_DIR_NONE);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    return obj;
}

/** @brief 创建一个只画指示段的圆弧（背景弧透明、不响应点击、无旋钮）。 */
static lv_obj_t *make_indicator_arc(lv_obj_t *parent, uint32_t color)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, ARC_SIZE, ARC_SIZE);
    lv_obj_center(arc);
    lv_obj_set_clickable(arc, false);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, ARC_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_arc_set_angles(arc, ARC_BOTTOM_DEG, ARC_BOTTOM_DEG);
    return arc;
}

/* -------------------------------------------------------------------------- */

static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    gmeter_ui_data_t d;
    taskENTER_CRITICAL(&s_snap_mux);
    d = s_snap;
    taskEXIT_CRITICAL(&s_snap_mux);

    /* 坐标映射：纵向(前后) → 屏幕上下，横向 → 屏幕左右 */
    const float long_a = d.lin[GMETER_UI_AXIS_LONG] * GMETER_UI_LONG_SIGN;
    const float lat_a = d.lin[GMETER_UI_AXIS_LAT] * GMETER_UI_LAT_SIGN;

    float tx = lat_a * GMETER_UI_SCALE_PX_PER_MPS2;
    float ty = -long_a * GMETER_UI_SCALE_PX_PER_MPS2;

    /* 限幅到圆内，避免超量程时气泡飞出屏幕 */
    const float len = sqrtf(tx * tx + ty * ty);
    if (len > (float)MOVE_R) {
        tx *= (float)MOVE_R / len;
        ty *= (float)MOVE_R / len;
    }

    /* 轻量平滑，让气泡移动不抖 */
    s_bx += 0.40f * (tx - s_bx);
    s_by += 0.40f * (ty - s_by);

    /*
     * 注意：气泡在创建时用的是 lv_obj_align(LV_ALIGN_CENTER)，此后
     * lv_obj_set_x/y 的取值是「相对中心」的偏移，不能用屏幕绝对坐标（会跑到屏幕外）。
     * 这里统一用 lv_obj_align 的中心相对定位。
     */
    lv_obj_align(s_bubble, LV_ALIGN_CENTER, (int32_t)s_bx, (int32_t)s_by);

    /*
     * 气泡颜色：红色需要"持续 200ms 超过 5 m/s²"，避免过坑的单次脉冲就闪红；
     * 低于阈值后按 2 倍速度回落。
     */
    if (d.mag >= GMETER_UI_RED_MPS2) {
        s_red_ms += 33;
    } else {
        s_red_ms -= 66;
        if (s_red_ms < 0) {
            s_red_ms = 0;
        }
    }

    uint32_t bubble_col = COL_OK;
    if (s_red_ms >= GMETER_UI_RED_HOLD_MS) {
        bubble_col = COL_DANGER;
    } else if (d.mag >= GMETER_UI_YELLOW_MPS2) {
        bubble_col = COL_WARN;
    }
    lv_obj_set_style_bg_color(s_bubble, lv_color_hex(bubble_col), 0);
    lv_obj_set_style_shadow_color(s_bubble, lv_color_hex(bubble_col), 0);

    /* 数值 */
    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f", (double)d.mag);
    lv_label_set_text(s_val, buf);

    /* 峰值 */
    if (d.mag > s_peak_mag) {
        s_peak_mag = d.mag;
    }
    snprintf(buf, sizeof(buf), "MAX %.1f", (double)s_peak_mag);
    lv_label_set_text(s_peak, buf);

    /*
     * 底部圆弧条：从 6 点钟方向（90°）向两侧生长，
     * 加速 → 绿色向右（角度递减），刹车 → 红色向左（角度递增），
     * 单侧满量程扫过 90°，即左右各占半个下半圆。
     */
    float span = 0.0f;
    const char *state = "IDLE";

    if (long_a > GMETER_UI_STATE_MPS2) {
        state = "ACCEL";
        span = long_a / GMETER_UI_FULL_SCALE_MPS2 * (float)ARC_HALF_SPAN;
    } else if (long_a < -GMETER_UI_STATE_MPS2) {
        state = "BRAKE";
        span = -long_a / GMETER_UI_FULL_SCALE_MPS2 * (float)ARC_HALF_SPAN;
    }
    if (span > (float)ARC_HALF_SPAN) {
        span = (float)ARC_HALF_SPAN;
    }

    /* 加速弧：90-span → 90（顺时针方向即右半边） */
    lv_arc_set_angles(s_arc_accel,
                      (lv_value_precise_t)(ARC_BOTTOM_DEG - (long_a > 0 ? span : 0)),
                      (lv_value_precise_t)ARC_BOTTOM_DEG);
    /* 刹车弧：90 → 90+span（左半边） */
    lv_arc_set_angles(s_arc_brake,
                      (lv_value_precise_t)ARC_BOTTOM_DEG,
                      (lv_value_precise_t)(ARC_BOTTOM_DEG + (long_a < 0 ? span : 0)));

    lv_label_set_text(s_state, state);

    /* 垂直/路面通道：平滑后的垂直加速度幅值（非负，所以用普通进度条） */
    const float vert = fabsf(d.lin[GMETER_UI_AXIS_VERT]);
    s_vert_avg += 0.10f * (vert - s_vert_avg);

    int32_t road_pct = (int32_t)(s_vert_avg / GMETER_UI_ROAD_FULL_MPS2 * 100.0f);
    if (road_pct > 100) {
        road_pct = 100;
    }
    lv_bar_set_value(s_road, road_pct, LV_ANIM_OFF);

    uint32_t road_col = 0x00BCD4; /* 青色：路面平稳 */
    if (road_pct > 70) {
        road_col = COL_DANGER;
    } else if (road_pct > 40) {
        road_col = COL_WARN;
    }
    lv_obj_set_style_bg_color(s_road, lv_color_hex(road_col), LV_PART_INDICATOR);

    /* 转向角速度（绕 Z） */
    snprintf(buf, sizeof(buf), "TURN %+.0f dps", (double)d.gyro[2]);
    lv_label_set_text(s_turn, buf);
}

/* -------------------------------------------------------------------------- */

esp_err_t gmeter_ui_start(void)
{
    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        ESP_LOGE(TAG, "bsp_display_start failed");
        return ESP_FAIL;
    }

    if (bsp_display_lock(1000) != ESP_OK) {
        ESP_LOGE(TAG, "failed to lock LVGL");
        return ESP_FAIL;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_scroll_dir(scr, LV_DIR_NONE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* 外圈 */
    lv_obj_t *ring = make_obj(scr, UI_SIZE - 16, UI_SIZE - 16, COL_BG, LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_width(ring, 2, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(COL_RING), 0);
    lv_obj_center(ring);

    /* 十字准线 */
    lv_obj_t *h_line = make_obj(scr, 120, 1, COL_CROSS, 0);
    lv_obj_align(h_line, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t *v_line = make_obj(scr, 1, 120, COL_CROSS, 0);
    lv_obj_align(v_line, LV_ALIGN_CENTER, 0, 0);

    /* 中心点 */
    lv_obj_t *center_dot = make_obj(scr, 6, 6, COL_CROSS, LV_RADIUS_CIRCLE);
    lv_obj_align(center_dot, LV_ALIGN_CENTER, 0, 0);

    /* 气泡 */
    s_bubble = make_obj(scr, BUBBLE_R * 2, BUBBLE_R * 2, COL_OK, LV_RADIUS_CIRCLE);
    lv_obj_set_style_shadow_width(s_bubble, 16, 0);
    lv_obj_set_style_shadow_opa(s_bubble, LV_OPA_50, 0);
    lv_obj_set_style_shadow_spread(s_bubble, 2, 0);
    lv_obj_align(s_bubble, LV_ALIGN_CENTER, 0, 0);

    /* 峰值（顶部） */
    s_peak = lv_label_create(scr);
    lv_obj_set_style_text_color(s_peak, lv_color_hex(0x888888), 0);
    lv_label_set_text(s_peak, "MAX 0.0");
    lv_obj_align(s_peak, LV_ALIGN_CENTER, 0, -150);

    /* 路面强度：顶部小进度条（垂直方向加速度的平滑值） */
    s_road = lv_bar_create(scr);
    lv_obj_set_size(s_road, 120, 8);
    lv_bar_set_range(s_road, 0, 100);
    lv_bar_set_value(s_road, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_road, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_road, lv_color_hex(0x00BCD4), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_road, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_road, 4, LV_PART_INDICATOR);
    lv_obj_align(s_road, LV_ALIGN_CENTER, 0, -172);

    lv_obj_t *road_label = lv_label_create(scr);
    lv_obj_set_style_text_color(road_label, lv_color_hex(0x666666), 0);
    lv_label_set_text(road_label, "ROAD");
    lv_obj_align(road_label, LV_ALIGN_CENTER, 0, -192);

    /* 转向角速度（顶部下方） */
    s_turn = lv_label_create(scr);
    lv_obj_set_style_text_color(s_turn, lv_color_hex(0x666666), 0);
    lv_label_set_text(s_turn, "TURN +0 dps");
    lv_obj_align(s_turn, LV_ALIGN_CENTER, 0, -120);

    /* 提示（标定中 / 就绪） */
    s_hint = lv_label_create(scr);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0xAAAAAA), 0);
    lv_label_set_text(s_hint, "READY");
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, -88);

    /* 大数值 */
    s_val = lv_label_create(scr);
    lv_obj_set_style_text_color(s_val, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_val, &lv_font_montserrat_40, 0);
    lv_label_set_text(s_val, "0.0");
    lv_obj_align(s_val, LV_ALIGN_CENTER, 0, 92);

    /* 单位 */
    s_unit = lv_label_create(scr);
    lv_obj_set_style_text_color(s_unit, lv_color_hex(0x777777), 0);
    lv_label_set_text(s_unit, "m/s2");
    lv_obj_align(s_unit, LV_ALIGN_CENTER, 0, 132);

    /* 状态 */
    s_state = lv_label_create(scr);
    lv_obj_set_style_text_color(s_state, lv_color_hex(0x999999), 0);
    lv_label_set_text(s_state, "IDLE");
    lv_obj_align(s_state, LV_ALIGN_CENTER, 0, 155);

    /* 底部圆弧：背景弧（淡灰，下半圆）+ 加速弧（绿）+ 刹车弧（红） */
    s_arc_bg = lv_arc_create(scr);
    lv_obj_set_size(s_arc_bg, ARC_SIZE, ARC_SIZE);
    lv_obj_center(s_arc_bg);
    lv_obj_set_clickable(s_arc_bg, false);
    lv_arc_set_bg_angles(s_arc_bg, 0, 180);
    lv_arc_set_angles(s_arc_bg, ARC_BOTTOM_DEG, ARC_BOTTOM_DEG);
    lv_obj_set_style_arc_width(s_arc_bg, ARC_W, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc_bg, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_arc_bg, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_arc_bg, LV_OPA_TRANSP, LV_PART_KNOB);

    s_arc_accel = make_indicator_arc(scr, COL_OK);
    s_arc_brake = make_indicator_arc(scr, COL_DANGER);

    /* 30fps 刷新（在 LVGL 任务中执行） */
    lv_timer_create(ui_timer_cb, 33, NULL);

    bsp_display_unlock();

    ESP_LOGI(TAG, "UI ready (%dx%d)", UI_SIZE, UI_SIZE);
    return ESP_OK;
}
