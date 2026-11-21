#include "music_ui.h"

#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "cjk_font.h"
#include "esp_log.h"
#include "lvgl.h"
#include "ui_bg.h"

#include "music_config.h"
#include "music_player.h"

static const char *TAG = "music.ui";

#define UI_LOCK_MS      1000
/* 触摸控制器上电时会给出一次假触摸，界面刚出现的这段时间忽略点击 */
#define CLICK_GUARD_MS  800

#define TEXT_PX         20
#define STATE_PX        22
#define SMALL_PX        14

static const lv_font_t *s_font_text = NULL;
static const lv_font_t *s_font_state = NULL;
static const lv_font_t *s_font_small = NULL;

static lv_obj_t *s_state = NULL;
static lv_obj_t *s_track = NULL;
static lv_obj_t *s_detail = NULL;
static lv_obj_t *s_bar = NULL;
static lv_obj_t *s_time_now = NULL;
static lv_obj_t *s_time_total = NULL;
static lv_obj_t *s_button = NULL;
static lv_obj_t *s_button_label = NULL;
static uint32_t s_created_tick = 0;

/* -------------------------------------------------------------------------- */

static void set_text_if_changed(lv_obj_t *label, const char *text)
{
    if (label == NULL) {
        return;
    }
    if (text == NULL) {
        text = "";
    }
    const char *current = lv_label_get_text(label);
    if (current != NULL && strcmp(current, text) == 0) {
        return;
    }
    lv_label_set_text(label, text);
}

static void format_time(char *buf, size_t len, uint32_t ms)
{
    const uint32_t total_s = ms / 1000;
    snprintf(buf, len, "%u:%02u", (unsigned)(total_s / 60), (unsigned)(total_s % 60));
}

/** @brief 按钮：播放 <-> 暂停 */
static void button_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (lv_tick_get() - s_created_tick < CLICK_GUARD_MS) {
        return;
    }
    music_player_toggle();
}

/**
 * @brief 每 200ms 拉一次播放器快照刷新界面。
 *
 * 这个定时器跑在 LVGL 任务里，和按钮回调同一个上下文，不需要再抢显示锁。
 */
static void refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    music_status_t st;
    music_player_status(&st);

    /* 投屏时状态行说明声音来自手机，避免用户以为是本地曲目 */
    const bool casting = (st.source == MUSIC_SOURCE_DLNA);
    if (casting && st.state == MUSIC_STATE_PLAYING) {
        set_text_if_changed(s_state, "投屏播放中");
    } else if (casting && st.state == MUSIC_STATE_PAUSED) {
        set_text_if_changed(s_state, "投屏已暂停");
    } else {
        set_text_if_changed(s_state, music_player_state_text(st.state));
    }

    uint32_t state_color = MUSIC_COL_ACCENT;
    if (st.state == MUSIC_STATE_PAUSED) {
        state_color = MUSIC_COL_WARN;
    } else if (st.state == MUSIC_STATE_ERROR || st.state == MUSIC_STATE_NO_TRACK) {
        state_color = MUSIC_COL_ERR;
    }
    lv_obj_set_style_text_color(s_state, lv_color_hex(state_color), 0);

    if (st.track[0] != '\0') {
        set_text_if_changed(s_track, st.track);
    }

    /* 有错误就占掉格式那一行；投屏时显示歌手；否则显示实际格式 */
    if (st.state == MUSIC_STATE_ERROR || st.state == MUSIC_STATE_NO_TRACK) {
        set_text_if_changed(s_detail, st.error[0] != '\0' ? st.error : "长按 BOOT 回选择器");
    } else if (casting) {
        char info[80];
        if (st.artist[0] != '\0') {
            snprintf(info, sizeof(info), "%s · 手机投屏", st.artist);
        } else {
            snprintf(info, sizeof(info), "手机投屏");
        }
        set_text_if_changed(s_detail, info);
    } else {
        char info[64];
        snprintf(info, sizeof(info), "%u Hz / %u bit / %s",
                 (unsigned)st.sample_rate, (unsigned)st.bits,
                 st.channels > 1 ? "立体声" : "单声道");
        set_text_if_changed(s_detail, info);
    }

    /* 进度 */
    int percent = 0;
    if (st.duration_ms > 0 && st.position_ms <= st.duration_ms) {
        percent = (int)((uint64_t)st.position_ms * 100 / st.duration_ms);
    }
    lv_bar_set_value(s_bar, percent, LV_ANIM_OFF);

    char now[16];
    char total[16];
    format_time(now, sizeof(now), st.position_ms);
    format_time(total, sizeof(total), st.duration_ms);
    set_text_if_changed(s_time_now, now);
    set_text_if_changed(s_time_total, total);

    /* 按钮图标/颜色 */
    const bool can_toggle = (st.state == MUSIC_STATE_PLAYING || st.state == MUSIC_STATE_PAUSED ||
                             st.state == MUSIC_STATE_STOPPED);
    set_text_if_changed(s_button_label,
                        st.state == MUSIC_STATE_PLAYING ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_obj_set_style_bg_color(s_button,
                              lv_color_hex(can_toggle ? MUSIC_COL_ACCENT : MUSIC_COL_CARD), 0);
    lv_obj_set_style_bg_opa(s_button, can_toggle ? LV_OPA_COVER : LV_OPA_60, 0);
}

/* -------------------------------------------------------------------------- */

esp_err_t music_ui_start(void)
{
    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        ESP_LOGE(TAG, "bsp_display_start failed");
        return ESP_FAIL;
    }

    if (bsp_display_lock(UI_LOCK_MS) != ESP_OK) {
        ESP_LOGE(TAG, "failed to lock LVGL");
        return ESP_FAIL;
    }

    if (cjk_font_init() != ESP_OK) {
        ESP_LOGW(TAG, "shared CJK font unavailable (storage/font-cjk.ttf), falling back");
    }
    s_font_text = cjk_font_or(TEXT_PX, &lv_font_montserrat_20);
    s_font_state = cjk_font_or(STATE_PX, &lv_font_montserrat_28);
    s_font_small = &lv_font_montserrat_14;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_scroll_dir(scr, LV_DIR_NONE);

    /* 与 selector / watch / xiaozhi 同一张 30° 斜向渐变（components/ui_bg） */
    ui_bg_apply(scr);

    /* 顶部标题 */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "音乐播放器");
    lv_obj_set_style_text_color(title, lv_color_hex(MUSIC_COL_TEXT), 0);
    lv_obj_set_style_text_font(title, s_font_state, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 54);

    /* 中间的圆形"唱片"：只用内置符号，不依赖图片素材 */
    lv_obj_t *disc = lv_obj_create(scr);
    lv_obj_set_size(disc, 160, 160);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, lv_color_hex(MUSIC_COL_CARD), 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(disc, 2, 0);
    lv_obj_set_style_border_color(disc, lv_color_hex(MUSIC_COL_ACCENT), 0);
    lv_obj_set_style_pad_all(disc, 0, 0);
    lv_obj_set_scrollable(disc, false);
    lv_obj_align(disc, LV_ALIGN_TOP_MID, 0, 92);

    lv_obj_t *note = lv_label_create(disc);
    lv_label_set_text(note, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(note, lv_color_hex(MUSIC_COL_ACCENT_2), 0);
    lv_obj_set_style_text_font(note, &lv_font_montserrat_40, 0);
    lv_obj_center(note);

    /* 播放状态 */
    s_state = lv_label_create(scr);
    lv_label_set_text(s_state, "");
    lv_obj_set_style_text_color(s_state, lv_color_hex(MUSIC_COL_ACCENT), 0);
    lv_obj_set_style_text_font(s_state, s_font_text, 0);
    lv_obj_align(s_state, LV_ALIGN_TOP_MID, 0, 258);

    /* 曲目名（文件名） */
    s_track = lv_label_create(scr);
    lv_label_set_text(s_track, "…");
    lv_obj_set_style_text_color(s_track, lv_color_hex(MUSIC_COL_TEXT), 0);
    lv_obj_set_style_text_font(s_track, s_font_text, 0);
    lv_label_set_long_mode(s_track, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_track, 340);
    lv_obj_set_style_text_align(s_track, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_track, LV_ALIGN_TOP_MID, 0, 284);

    /* 格式 / 错误提示 */
    s_detail = lv_label_create(scr);
    lv_label_set_text(s_detail, "");
    lv_obj_set_style_text_color(s_detail, lv_color_hex(MUSIC_COL_MUTED), 0);
    lv_obj_set_style_text_font(s_detail, s_font_text, 0);
    lv_label_set_long_mode(s_detail, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_detail, 360);
    lv_obj_set_style_text_align(s_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_detail, LV_ALIGN_TOP_MID, 0, 310);

    /* 进度条 */
    s_bar = lv_bar_create(scr);
    lv_obj_set_size(s_bar, 300, 8);
    lv_obj_set_style_radius(s_bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(MUSIC_COL_TRACK), 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(MUSIC_COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_align(s_bar, LV_ALIGN_TOP_MID, 0, 340);

    s_time_now = lv_label_create(scr);
    lv_label_set_text(s_time_now, "0:00");
    lv_obj_set_style_text_color(s_time_now, lv_color_hex(MUSIC_COL_MUTED), 0);
    lv_obj_set_style_text_font(s_time_now, s_font_small, 0);
    lv_obj_align(s_time_now, LV_ALIGN_TOP_LEFT, 78, 354);

    s_time_total = lv_label_create(scr);
    lv_label_set_text(s_time_total, "0:00");
    lv_obj_set_style_text_color(s_time_total, lv_color_hex(MUSIC_COL_MUTED), 0);
    lv_obj_set_style_text_font(s_time_total, s_font_small, 0);
    lv_obj_align(s_time_total, LV_ALIGN_TOP_RIGHT, -78, 354);

    /* 播放 / 暂停按钮 */
    s_button = lv_obj_create(scr);
    lv_obj_set_size(s_button, 80, 80);
    lv_obj_set_style_radius(s_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_button, lv_color_hex(MUSIC_COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_button, 0, 0);
    lv_obj_set_style_pad_all(s_button, 0, 0);
    lv_obj_set_scrollable(s_button, false);
    lv_obj_align(s_button, LV_ALIGN_BOTTOM_MID, 0, -38);
    lv_obj_add_event_cb(s_button, button_event_cb, LV_EVENT_CLICKED, NULL);

    s_button_label = lv_label_create(s_button);
    lv_label_set_text(s_button_label, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(s_button_label, lv_color_hex(MUSIC_COL_TEXT), 0);
    lv_obj_set_style_text_font(s_button_label, &lv_font_montserrat_28, 0);
    lv_obj_center(s_button_label);

    s_created_tick = lv_tick_get();
    lv_timer_create(refresh_timer_cb, MUSIC_UI_TICK_MS, NULL);
    refresh_timer_cb(NULL);

    bsp_display_unlock();
    ESP_LOGI(TAG, "ui ready");
    return ESP_OK;
}
