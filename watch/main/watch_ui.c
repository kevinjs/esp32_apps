#include "watch_ui.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "cjk_font.h"
#include "tap_gesture.h"
#include "ui_bg.h"
#include "watch_config.h"
#include "watch_stopwatch.h"
#include "watch_time.h"

static const char *TAG = "watch.ui";

#define UI_SIZE     466
#define BTN_D       88          /* 秒表圆按钮直径 */
#define BTN_GAP_X   104         /* 三个按钮左右间距（间距 - 直径 = 16 px） */
#define BTN_Y       110         /* 按钮所在高度：底边 154，给页码点留空间 */
#define LAP_ROW_W   240         /* 圈速行宽度 */
#define LAP_ROW_H   20
#define LAP_TOP_Y   (-42)       /* 第一行圈速的顶边（相对中心），整块 -42~58 */

#define DEG2RAD     0.017453292519943295f

/* 指针 / 刻度的画布：边长 FACE_BOX，圆心在 (FACE_BOX/2, FACE_BOX/2)。
 * 必须 ≥ 2×最长刻度半径（205），否则超出部分会被控件裁剪。 */
#define FACE_BOX        420
#define TICK_R_OUT      205     /* 刻度外端 */
#define TICK_R_MAJOR_IN 182     /* 12/3/6/9 点刻度内端（长刻度） */
#define TICK_R_MINOR_IN 192     /* 其它小时刻度内端（短刻度） */
#define NUMERAL_R       166     /* 整点数字所在半径 */

#define HAND_HOUR_LEN   90
#define HAND_MIN_LEN    126
#define HAND_SEC_LEN    148
#define HAND_HOUR_TAIL  12      /* 尾端反向伸出，做配重 */
#define HAND_MIN_TAIL   16
#define HAND_SEC_TAIL   22

/* 机械表式日期窗：夹在圆心与 3 点钟数字之间，两行（日期 / 星期） */
#define WIN_X           108     /* 窗口中心相对表盘中心向右偏移 */
#define WIN_W           84
#define WIN_H           42

/* 页码点：放在整点数字（r=166）与 6 点钟长刻度之间，左右错开避开刻度线 */
#define PAGE_DOT_Y      192
#define PAGE_DOT_GAP    18

/* -------------------------------------------------------------------------- */
/*  对象句柄                                                                   */
/* -------------------------------------------------------------------------- */

static lv_obj_t *s_pager;               /* 横向滑动容器，两个页面并排 */
static lv_obj_t *s_dots[2];

/* 表盘 */
static lv_obj_t *s_face_date;   /* 日期窗第一行：11月20日 */
static lv_obj_t *s_face_wday;   /* 日期窗第二行：周五 */

/* 表盘指针 / 刻度：lv_line 只保存传入的数组指针，点数组必须是静态的 */
static lv_obj_t *s_hand_hour;
static lv_obj_t *s_hand_min;
static lv_obj_t *s_hand_sec;
static lv_point_precise_t s_pts_hour[2];
static lv_point_precise_t s_pts_min[2];
static lv_point_precise_t s_pts_sec[2];
static lv_point_precise_t s_tick_pts[12][2];
static int s_last_sec = -1;     /* 上一次画指针时的秒；-1 = 还没画过 */

/* 秒表 */
static lv_obj_t *s_sw_elapsed;
static lv_obj_t *s_sw_state;
static lv_obj_t *s_sw_primary;
static lv_obj_t *s_sw_primary_label;
static lv_obj_t *s_sw_lap_rows[WATCH_MAX_LAPS];
static lv_obj_t *s_sw_lap_split[WATCH_MAX_LAPS];
static lv_obj_t *s_sw_lap_total[WATCH_MAX_LAPS];

static const char *const WDAY_CN[7] = { "日", "一", "二", "三", "四", "五", "六" };

/* -------------------------------------------------------------------------- */
/*  小工具                                                                     */
/* -------------------------------------------------------------------------- */

/** @brief 文本没变就不碰 label，避免每帧都触发重绘 */
static void label_set(lv_obj_t *label, const char *text)
{
    if (label == NULL || text == NULL) {
        return;
    }
    const char *cur = lv_label_get_text(label);
    if (cur != NULL && strcmp(cur, text) == 0) {
        return;
    }
    lv_label_set_text(label, text);
}

static lv_obj_t *dot_create(lv_obj_t *parent, int32_t diameter, uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_clickable(obj, false);
    lv_obj_set_size(obj, diameter, diameter);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    return obj;
}

static lv_obj_t *label_create(lv_obj_t *parent, const char *text,
                              const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

/** @brief 与视口等大的透明页，关掉自身滚动，手势交给 pager */
static lv_obj_t *page_create(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_size(page, UI_SIZE, UI_SIZE);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_scrollable(page, false);
    return page;
}

/** @brief 计时值 → "MM:SS.CC"；超过 1 小时自动换成 "H:MM:SS" */
static void fmt_duration(int64_t us, char *buf, size_t len)
{
    int64_t cs = us / 10000;            /* 百分秒 */
    if (cs < 0) {
        cs = 0;
    }

    const int64_t hours = cs / 360000;
    cs %= 360000;
    const int minutes = (int)(cs / 6000);
    cs %= 6000;
    const int seconds = (int)(cs / 100);
    const int centis = (int)(cs % 100);

    if (hours > 0) {
        snprintf(buf, len, "%lld:%02d:%02d", (long long)hours, minutes, seconds);
    } else {
        snprintf(buf, len, "%02d:%02d.%02d", minutes, seconds, centis);
    }
}

static void dots_update(int page)
{
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_bg_color(s_dots[i],
                                  lv_color_hex(i == page ? WATCH_COL_ACCENT : WATCH_COL_IDLE), 0);
    }
}

/* -------------------------------------------------------------------------- */
/*  按钮回调（全部转成 watch_stopwatch_* 调用）                                 */
/* -------------------------------------------------------------------------- */

static void on_toggle_released(lv_event_t *event)
{
    (void)event;
    if (!tap_gesture_was_tap()) {
        return;     /* 拖动翻页时误触，忽略 */
    }
    watch_stopwatch_toggle();
}

static void on_lap_released(lv_event_t *event)
{
    (void)event;
    if (!tap_gesture_was_tap()) {
        return;
    }
    watch_stopwatch_lap();
}

static void on_reset_released(lv_event_t *event)
{
    (void)event;
    if (!tap_gesture_was_tap()) {
        return;
    }
    watch_stopwatch_reset();
}

/* -------------------------------------------------------------------------- */
/*  表盘页                                                                     */
/* -------------------------------------------------------------------------- */

/**
 * @brief 把指针摆到指定角度（0° = 12 点，顺时针）
 *
 * 点坐标相对 lv_line 自身的左上角，所以画布按 FACE_BOX 建、圆心固定在正中。
 */
static void hand_set(lv_obj_t *line, lv_point_precise_t *pts,
                     float deg, int32_t len, int32_t tail)
{
    const float rad = deg * DEG2RAD;
    const float s = sinf(rad);
    const float c = cosf(rad);
    const int32_t cx = FACE_BOX / 2;
    const int32_t cy = FACE_BOX / 2;

    pts[0].x = cx - (int32_t)lroundf(s * (float)tail);   /* 尾端（反向一小段） */
    pts[0].y = cy + (int32_t)lroundf(c * (float)tail);
    pts[1].x = cx + (int32_t)lroundf(s * (float)len);    /* 针尖 */
    pts[1].y = cy - (int32_t)lroundf(c * (float)len);

    lv_line_set_points(line, pts, 2);
}

static lv_obj_t *hand_create(lv_obj_t *page, lv_point_precise_t *pts,
                             int32_t width, uint32_t color, int32_t len, int32_t tail)
{
    lv_obj_t *line = lv_line_create(page);
    lv_obj_set_size(line, FACE_BOX, FACE_BOX);
    lv_obj_align(line, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_scrollable(line, false);
    lv_obj_set_clickable(line, false);
    lv_obj_set_style_bg_opa(line, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(color), 0);
    lv_obj_set_style_line_rounded(line, true, 0);

    hand_set(line, pts, 0.0f, len, tail);   /* 先摆到 12 点，避免首帧空指针 */
    return line;
}

/** @brief 画一条从 r_in 到 r_out 的小时刻度线（lv_line，点数组静态保存） */
static void tick_create(lv_obj_t *page, int idx, int32_t r_in, int32_t width, uint32_t color)
{
    lv_obj_t *line = lv_line_create(page);
    lv_obj_set_size(line, FACE_BOX, FACE_BOX);
    lv_obj_align(line, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_scrollable(line, false);
    lv_obj_set_clickable(line, false);
    lv_obj_set_style_bg_opa(line, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(color), 0);
    lv_obj_set_style_line_rounded(line, true, 0);

    const float rad = (float)idx * 30.0f * DEG2RAD;
    const float s = sinf(rad);
    const float c = cosf(rad);
    const int32_t cx = FACE_BOX / 2;
    const int32_t cy = FACE_BOX / 2;

    s_tick_pts[idx][0].x = cx + (int32_t)lroundf(s * (float)r_in);
    s_tick_pts[idx][0].y = cy - (int32_t)lroundf(c * (float)r_in);
    s_tick_pts[idx][1].x = cx + (int32_t)lroundf(s * (float)TICK_R_OUT);
    s_tick_pts[idx][1].y = cy - (int32_t)lroundf(c * (float)TICK_R_OUT);

    lv_line_set_points(line, s_tick_pts[idx], 2);
}

static void face_create(lv_obj_t *page)
{
    /* 表圈：只留一圈细边，底色透明——里面要透出整屏的斜向渐变 */
    lv_obj_t *bezel = lv_obj_create(page);
    lv_obj_set_scrollable(bezel, false);
    lv_obj_set_clickable(bezel, false);
    lv_obj_set_size(bezel, UI_SIZE - 14, UI_SIZE - 14);
    lv_obj_set_style_radius(bezel, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(bezel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bezel, 2, 0);
    lv_obj_set_style_border_color(bezel, lv_color_hex(0x2A2A3A), 0);
    lv_obj_set_style_pad_all(bezel, 0, 0);
    lv_obj_center(bezel);

    /* 12 条放射状刻度：3/6/9/12 点更长更粗（白），其余短而暗 */
    for (int i = 0; i < 12; i++) {
        const bool major = (i % 3) == 0;
        tick_create(page, i,
                    major ? TICK_R_MAJOR_IN : TICK_R_MINOR_IN,
                    major ? 4 : 2,
                    major ? WATCH_COL_TEXT : 0x4A4A55);
    }

    /* 整点数字 12/3/6/9/…，压在刻度圈内侧 */
    for (int i = 0; i < 12; i++) {
        const int hour = (i == 0) ? 12 : i;
        const bool major = (i % 3) == 0;
        char num[4];
        snprintf(num, sizeof(num), "%d", hour);

        const float rad = (float)i * 30.0f * DEG2RAD;
        lv_obj_t *label = label_create(page, num,
                                       major ? &lv_font_montserrat_20 : &lv_font_montserrat_16,
                                       major ? WATCH_COL_TEXT : WATCH_COL_DIM);
        lv_obj_align(label, LV_ALIGN_CENTER,
                     (int32_t)lroundf(sinf(rad) * (float)NUMERAL_R),
                     (int32_t)lroundf(-cosf(rad) * (float)NUMERAL_R));
    }

    /*
     * 机械表式日期窗：夹在圆心与 3 点钟数字之间，两行显示
     * 「11月20日」+「周五」；时间不可信时第一行变成「等待校时」。
     */
    lv_obj_t *win = lv_obj_create(page);
    lv_obj_set_scrollable(win, false);
    lv_obj_set_clickable(win, false);
    lv_obj_set_size(win, WIN_W, WIN_H);
    lv_obj_set_style_radius(win, 12, 0);
    lv_obj_set_style_bg_color(win, lv_color_hex(0x0A0A14), 0);
    lv_obj_set_style_bg_opa(win, 220, 0);
    lv_obj_set_style_border_width(win, 1, 0);
    lv_obj_set_style_border_color(win, lv_color_hex(WATCH_COL_ACCENT), 0);
    lv_obj_set_style_border_opa(win, 140, 0);
    lv_obj_set_style_pad_all(win, 0, 0);
    lv_obj_align(win, LV_ALIGN_CENTER, WIN_X, 0);

    s_face_date = label_create(win, "----月--日", cjk_font_or(16, &lv_font_montserrat_16),
                               WATCH_COL_TEXT);
    lv_obj_align(s_face_date, LV_ALIGN_CENTER, 0, -10);

    s_face_wday = label_create(win, "--", cjk_font_or(14, &lv_font_montserrat_14),
                               WATCH_COL_ACCENT);
    lv_obj_align(s_face_wday, LV_ALIGN_CENTER, 0, 10);

    /* 指针：时针最粗最短，分针次之，秒针细且用强调色 */
    s_hand_hour = hand_create(page, s_pts_hour, 8, WATCH_COL_TEXT, HAND_HOUR_LEN, HAND_HOUR_TAIL);
    s_hand_min = hand_create(page, s_pts_min, 6, WATCH_COL_TEXT, HAND_MIN_LEN, HAND_MIN_TAIL);
    s_hand_sec = hand_create(page, s_pts_sec, 2, WATCH_COL_ACCENT, HAND_SEC_LEN, HAND_SEC_TAIL);

    /* 中心轴帽画在指针之后，盖住三根针的交点 */
    lv_obj_t *hub = dot_create(page, 14, WATCH_COL_TEXT);
    lv_obj_align(hub, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t *hub_dot = dot_create(page, 6, WATCH_COL_ACCENT);
    lv_obj_align(hub_dot, LV_ALIGN_CENTER, 0, 0);
}

/* -------------------------------------------------------------------------- */
/*  秒表页                                                                     */
/* -------------------------------------------------------------------------- */

static lv_obj_t *button_create(lv_obj_t *parent, const char *text, int32_t x, int32_t y,
                               uint32_t color, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_scrollable(btn, false);
    lv_obj_set_size(btn, BTN_D, BTN_D);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x141414), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_align(btn, LV_ALIGN_CENTER, x, y);

    lv_obj_t *label = label_create(btn, text, cjk_font_or(20, &lv_font_montserrat_20), color);
    lv_obj_center(label);

    /* 从按钮按下、拖到别处松手不算点击 */
    tap_gesture_attach(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_RELEASED, NULL);

    return btn;
}

static void stopwatch_create(lv_obj_t *page)
{
    lv_obj_t *caption = label_create(page, "秒  表", cjk_font_or(14, &lv_font_montserrat_14),
                                     WATCH_COL_DIM);
    lv_obj_align(caption, LV_ALIGN_CENTER, 0, -176);

    s_sw_elapsed = label_create(page, "00:00.00", &lv_font_montserrat_40, WATCH_COL_TEXT);
    lv_obj_align(s_sw_elapsed, LV_ALIGN_CENTER, 0, -118);

    s_sw_state = label_create(page, "就绪", cjk_font_or(14, &lv_font_montserrat_14), WATCH_COL_DIM);
    lv_obj_align(s_sw_state, LV_ALIGN_CENTER, 0, -66);

    /* 圈速列表：左边分段、右边累计，最多 WATCH_MAX_LAPS 行 */
    for (int i = 0; i < WATCH_MAX_LAPS; i++) {
        lv_obj_t *row = lv_obj_create(page);
        lv_obj_set_scrollable(row, false);
        lv_obj_set_clickable(row, false);
        lv_obj_set_size(row, LAP_ROW_W, LAP_ROW_H);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_align(row, LV_ALIGN_CENTER, 0, LAP_TOP_Y + LAP_ROW_H / 2 + i * LAP_ROW_H);
        lv_obj_set_hidden(row, true);

        char idx[8];
        snprintf(idx, sizeof(idx), "%d", i + 1);
        lv_obj_t *num = label_create(row, idx, &lv_font_montserrat_14, WATCH_COL_DIM);
        lv_obj_align(num, LV_ALIGN_LEFT_MID, 6, 0);

        s_sw_lap_split[i] = label_create(row, "--:--.--", &lv_font_montserrat_14, WATCH_COL_TEXT);
        lv_obj_align(s_sw_lap_split[i], LV_ALIGN_LEFT_MID, 26, 0);

        s_sw_lap_total[i] = label_create(row, "--:--.--", &lv_font_montserrat_14, WATCH_COL_DIM);
        lv_obj_align(s_sw_lap_total[i], LV_ALIGN_RIGHT_MID, -6, 0);

        s_sw_lap_rows[i] = row;
    }

    s_sw_primary = button_create(page, "开始", -BTN_GAP_X, BTN_Y, WATCH_COL_OK, on_toggle_released);
    s_sw_primary_label = lv_obj_get_child(s_sw_primary, 0);
    button_create(page, "记圈", 0, BTN_Y, WATCH_COL_ACCENT, on_lap_released);
    button_create(page, "复位", BTN_GAP_X, BTN_Y, WATCH_COL_DIM, on_reset_released);
}

/* -------------------------------------------------------------------------- */
/*  页面切换                                                                   */
/* -------------------------------------------------------------------------- */

static void on_pager_scroll_end(lv_event_t *event)
{
    (void)event;

    const int32_t x = lv_obj_get_scroll_x(s_pager);
    int page = (x + UI_SIZE / 2) / UI_SIZE;
    if (page < 0) {
        page = 0;
    } else if (page > 1) {
        page = 1;
    }

    /* 吸附可能停在整页 ±1~2 px 上；统一对齐到整像素，
     * 否则"整页"里会看到内容横向偏移几个像素 */
    const int32_t target = page * UI_SIZE;
    if (x != target) {
        lv_obj_scroll_to_x(s_pager, target, LV_ANIM_OFF);
    }

    dots_update(page);
}

void watch_ui_show_page(int page)
{
    if (s_pager == NULL) {
        return;
    }
    if (page < 0) {
        page = 0;
    } else if (page > 1) {
        page = 1;
    }

    if (bsp_display_lock(200) != ESP_OK) {
        ESP_LOGW(TAG, "lock timeout, page switch skipped");
        return;
    }

    const int32_t target = page * UI_SIZE;
    if (lv_obj_get_scroll_x(s_pager) == target) {
        /* 已经在目标页：绝不能再触发一次滚动动画，
         * 否则 BOOT 键每按一次，吸附动画就会把内容拖偏几像素 */
        dots_update(page);
        bsp_display_unlock();
        return;
    }

    lv_obj_scroll_to_x(s_pager, target, LV_ANIM_ON);
    dots_update(page);
    bsp_display_unlock();
}

void watch_ui_show_stopwatch(void)
{
    watch_ui_show_page(WATCH_PAGE_STOPWATCH);
}

/* -------------------------------------------------------------------------- */
/*  100ms 刷新                                                                 */
/* -------------------------------------------------------------------------- */

static void tick_cb(lv_timer_t *timer)
{
    (void)timer;

    char buf[32];

    /* ------------------------------ 表盘 ------------------------------ */
    struct tm now = { 0 };
    const bool valid = watch_time_get(&now);

    if (valid) {
        snprintf(buf, sizeof(buf), "%d月%d日", now.tm_mon + 1, now.tm_mday);
        label_set(s_face_date, buf);
        lv_obj_set_style_text_color(s_face_date, lv_color_hex(WATCH_COL_TEXT), 0);

        snprintf(buf, sizeof(buf), "周%s", WDAY_CN[now.tm_wday % 7]);
        label_set(s_face_wday, buf);

        /* 指针只在秒变化时重算（石英表式跳秒），100ms 轮询也不做无谓重绘 */
        if (now.tm_sec != s_last_sec) {
            s_last_sec = now.tm_sec;

            const float hour_deg = (float)(now.tm_hour % 12) * 30.0f + (float)now.tm_min * 0.5f;
            const float min_deg = (float)now.tm_min * 6.0f + (float)now.tm_sec * 0.1f;
            const float sec_deg = (float)now.tm_sec * 6.0f;

            hand_set(s_hand_hour, s_pts_hour, hour_deg, HAND_HOUR_LEN, HAND_HOUR_TAIL);
            hand_set(s_hand_min, s_pts_min, min_deg, HAND_MIN_LEN, HAND_MIN_TAIL);
            hand_set(s_hand_sec, s_pts_sec, sec_deg, HAND_SEC_LEN, HAND_SEC_TAIL);
        }

        lv_obj_set_hidden(s_hand_hour, false);
        lv_obj_set_hidden(s_hand_min, false);
        lv_obj_set_hidden(s_hand_sec, false);
    } else {
        /* 时间不可信：日期窗改说「等待校时」，并把指针藏起来
         * （摆在 12 点会让人误以为是零点） */
        s_last_sec = -1;
        label_set(s_face_date, "等待校时");
        lv_obj_set_style_text_color(s_face_date, lv_color_hex(WATCH_COL_WARN), 0);
        label_set(s_face_wday, "--");
        lv_obj_set_hidden(s_hand_hour, true);
        lv_obj_set_hidden(s_hand_min, true);
        lv_obj_set_hidden(s_hand_sec, true);
    }

    /* ------------------------------ 秒表 ------------------------------ */
    watch_stopwatch_snapshot_t snap;
    watch_stopwatch_snapshot(&snap);

    fmt_duration(snap.elapsed_us, buf, sizeof(buf));
    label_set(s_sw_elapsed, buf);

    const bool started = snap.running || snap.elapsed_us > 0;

    label_set(s_sw_state, snap.running ? "计时中" : (started ? "已暂停" : "就绪"));
    lv_obj_set_style_text_color(s_sw_state,
                               lv_color_hex(snap.running ? WATCH_COL_OK : WATCH_COL_DIM), 0);

    /* 主按钮跟着当前动作变色：能"开始/继续"是绿色，能"暂停"是黄色 */
    const uint32_t primary_col = snap.running ? WATCH_COL_WARN : WATCH_COL_OK;
    label_set(s_sw_primary_label, snap.running ? "暂停" : (started ? "继续" : "开始"));
    lv_obj_set_style_border_color(s_sw_primary, lv_color_hex(primary_col), 0);
    lv_obj_set_style_text_color(s_sw_primary_label, lv_color_hex(primary_col), 0);

    /* 圈速：没有的段直接藏起来，界面不跟着数据重排 */
    for (int i = 0; i < WATCH_MAX_LAPS; i++) {
        if (i < snap.lap_count) {
            lv_obj_set_hidden(s_sw_lap_rows[i], false);

            fmt_duration(snap.lap_split_us[i], buf, sizeof(buf));
            label_set(s_sw_lap_split[i], buf);

            fmt_duration(snap.lap_total_us[i], buf, sizeof(buf));
            label_set(s_sw_lap_total[i], buf);
        } else {
            lv_obj_set_hidden(s_sw_lap_rows[i], true);
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  入口                                                                       */
/* -------------------------------------------------------------------------- */

esp_err_t watch_ui_start(void)
{
    if (bsp_display_start() == NULL) {
        ESP_LOGE(TAG, "bsp_display_start failed");
        return ESP_FAIL;
    }

    /* 字库在 storage 分区，必须在 lv_init() 之后、创建 label 之前加载 */
    if (cjk_font_init() != ESP_OK) {
        ESP_LOGW(TAG, "CJK font unavailable, UI falls back to Montserrat");
    }

    if (bsp_display_lock(1000) != ESP_OK) {
        ESP_LOGE(TAG, "timeout taking LVGL lock");
        return ESP_FAIL;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_scrollable(scr, false);

    /* 背景与 selector 一致：同一张 30° 斜向渐变图（components/ui_bg） */
    ui_bg_apply(scr);

    /*
     * 横向分页：两页并排放在一个只能左右滚的容器里，
     * 靠 SCROLL_SNAP 让松手后自动对齐到整页，比自己做手势识别稳。
     */
    s_pager = lv_obj_create(scr);
    lv_obj_set_size(s_pager, UI_SIZE, UI_SIZE);
    lv_obj_center(s_pager);
    lv_obj_set_style_bg_opa(s_pager, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pager, 0, 0);
    lv_obj_set_style_radius(s_pager, 0, 0);
    lv_obj_set_style_pad_all(s_pager, 0, 0);
    lv_obj_set_scroll_dir(s_pager, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(s_pager, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(s_pager, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(s_pager, LV_FLEX_FLOW_ROW);
    lv_obj_add_event_cb(s_pager, on_pager_scroll_end, LV_EVENT_SCROLL_END, NULL);

    face_create(page_create(s_pager));
    stopwatch_create(page_create(s_pager));

    /* 页码点：画在 pager 之后，保证盖在上面且不跟着滑动；
     * y 收到 186 并左右错开，避开 6 点钟方向的刻度点（y=200） */
    for (int i = 0; i < 2; i++) {
        s_dots[i] = dot_create(scr, 6, WATCH_COL_IDLE);
        lv_obj_align(s_dots[i], LV_ALIGN_CENTER, (i == 0) ? -PAGE_DOT_GAP : PAGE_DOT_GAP,
                     PAGE_DOT_Y);
    }
    dots_update(WATCH_PAGE_FACE);

    lv_timer_create(tick_cb, WATCH_TICK_MS, NULL);

    bsp_display_unlock();

    ESP_LOGI(TAG, "UI ready (%dx%d)", UI_SIZE, UI_SIZE);
    return ESP_OK;
}
