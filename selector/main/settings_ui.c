/*
 * 设置页：与应用列表平级的入口，当前收纳 Wi-Fi 设置
 *
 * 页面只创建一次，关闭只是隐藏；再加设置项时在 create_root() 里追加行。
 */
#include "settings_ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"

#include "bsp/esp-bsp.h"

#include "app_settings.h"
#include "cjk_font.h"
#include "image_store.h"
#include "storage_ui.h"
#include "tap_gesture.h"
#include "ui_scroll_fx.h"
#include "ui_theme.h"
#include "wifi_config.h"
#include "wifi_setup.h"

static const char *TAG = "selector.settings";

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_wifi_sub = NULL;
static lv_obj_t *s_storage_sub = NULL;
static lv_obj_t *s_storage_bar = NULL;
static lv_obj_t *s_vol_slider = NULL;
static lv_obj_t *s_vol_label = NULL;
static lv_obj_t *s_bright_slider = NULL;
static lv_obj_t *s_bright_label = NULL;
static int s_last_bright_apply_ms = 0;

/* 拖动亮度时最短的写屏间隔：见 brightness_changed_cb 的说明 */
#define BRIGHT_APPLY_MIN_MS  60

/*
 * 列表的滚动效果：正对屏幕中心的那一行放大到 112%，其它的缩到 89%；
 * 上下边缘留一条渐隐带，内容滑进标题下面时化掉，不再被容器边缘硬裁。
 */
static const ui_scroll_fx_cfg_t SETTINGS_FX = {
    .fade_top = 56,
    .fade_bottom = 48,
    .focus_scale = 288,
    .idle_scale = 228,
    .focus_opa = LV_OPA_COVER,
    .idle_opa = 225,
};

/* LVGL 自带 snprintf 不支持 %f（CONFIG_LV_USE_FLOAT=n），小数位用整数算 */
static void format_mb(char *buf, size_t len, size_t bytes)
{
    const size_t unit = 1024u * 1024u;
    snprintf(buf, len, "%u.%u",
             (unsigned)(bytes / unit),
             (unsigned)(((bytes % unit) * 10u) / unit));
}

/* -------------------------------------------------------------------------- */

static void back_clicked_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_RELEASED || s_root == NULL) {
        return;
    }
    if (!tap_gesture_was_tap()) {
        return;
    }
    lv_obj_set_hidden(s_root, true);
}

static void wifi_clicked_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_RELEASED) {
        return;
    }
    if (!tap_gesture_was_tap()) {
        return;
    }
    ESP_LOGI(TAG, "open Wi-Fi setup");
    wifi_setup_open();
}

/* -------------------------------------------------------------------------- */

static void refresh_storage(void)
{
    if (s_storage_bar == NULL) {
        return;
    }

    size_t total = 0;
    size_t used = 0;
    image_store_info(&total, &used);

    const unsigned pct = (total == 0) ? 0u : (unsigned)(((uint64_t)used * 100u) / total);
    lv_bar_set_value(s_storage_bar, (int32_t)pct, LV_ANIM_OFF);

    char used_mb[12];
    char total_mb[12];
    format_mb(used_mb, sizeof(used_mb), used);
    format_mb(total_mb, sizeof(total_mb), total);

    char buf[64];
    snprintf(buf, sizeof(buf), "%s / %s MB  %u%%", used_mb, total_mb, pct);
    lv_label_set_text(s_storage_sub, buf);
    ESP_LOGI(TAG, "storage %s", buf);
}

static void refresh_wifi_subtitle(void)
{
    if (s_wifi_sub == NULL) {
        return;
    }

    char buf[96];
    const wifi_config_state_t state = wifi_config_get_state();
    wifi_cred_t cred;

    if (state == WIFI_CFG_STATE_CONNECTED) {
        snprintf(buf, sizeof(buf), "已连接  %s", wifi_config_get_ip());
    } else if (state == WIFI_CFG_STATE_CONNECTING) {
        snprintf(buf, sizeof(buf), "连接中…");
    } else if (wifi_config_load(&cred) != ESP_OK) {
        snprintf(buf, sizeof(buf), "未配置，点这里扫描");
    } else {
        snprintf(buf, sizeof(buf), "%s 未连接", cred.ssid);
    }

    lv_label_set_text(s_wifi_sub, buf);
}

/**
 * 一行设置项：图标 + 标题 + 副标题，整行可点
 *
 * @param icon  LVGL 内置图标的 UTF-8 串（LV_SYMBOL_*），用 Montserrat 渲染；
 *              中文字体里没有这些字形，不能用 cjk_font_or()。
 */
static lv_obj_t *make_row(
    lv_obj_t *parent,
    const char *icon,
    const char *title,
    lv_event_cb_t cb,
    lv_obj_t **out_subtitle
)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 300, 64);
    /* 无底色无边框：内容直接坐在背景渐变上，放大/渐隐时不会露出卡片边线 */
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_outline_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_clickable(row, true);
    tap_gesture_attach(row);
    lv_obj_add_event_cb(row, cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *icon_label = lv_label_create(row);
    lv_label_set_text(icon_label, icon);
    lv_obj_set_style_text_color(icon_label, lv_color_hex(UI_COLOR_ACCENT_2), 0);
    lv_obj_set_style_text_font(icon_label, &lv_font_montserrat_20, 0);
    lv_obj_align(icon_label, LV_ALIGN_LEFT_MID, 18, 0);

    lv_obj_t *title_label = lv_label_create(row);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(title_label, cjk_font_or(18, &lv_font_montserrat_16), 0);
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 62, -10);

    lv_obj_t *sub = lv_label_create(row);
    lv_label_set_text(sub, "");
    lv_obj_set_style_text_color(sub, lv_color_hex(UI_COLOR_MUTED), 0);
    lv_obj_set_style_text_font(sub, cjk_font_or(14, &lv_font_montserrat_14), 0);
    lv_obj_align(sub, LV_ALIGN_LEFT_MID, 62, 12);

    lv_obj_t *arrow = lv_label_create(row);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(arrow, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_16, 0);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -16, 0);

    if (out_subtitle != NULL) {
        *out_subtitle = sub;
    }
    return row;
}

static void storage_clicked_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_RELEASED) {
        return;
    }
    if (!tap_gesture_was_tap()) {
        return;
    }
    ESP_LOGI(TAG, "open storage details");
    storage_ui_open();
}

/* 存储行：图标 + 标题 + 用量文字 + 进度条，点进去看 app 列表与占用 */
static void make_storage_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 300, 88);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_outline_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_clickable(row, true);
    tap_gesture_attach(row);
    lv_obj_add_event_cb(row, storage_clicked_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text(icon, LV_SYMBOL_SD_CARD);
    lv_obj_set_style_text_color(icon, lv_color_hex(UI_COLOR_ACCENT_2), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 18, -6);

    lv_obj_t *title = lv_label_create(row);
    lv_label_set_text(title, "存储");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(title, cjk_font_or(18, &lv_font_montserrat_16), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 62, -26);

    s_storage_sub = lv_label_create(row);
    lv_label_set_text(s_storage_sub, "");
    lv_obj_set_style_text_color(s_storage_sub, lv_color_hex(UI_COLOR_MUTED), 0);
    lv_obj_set_style_text_font(s_storage_sub, cjk_font_or(14, &lv_font_montserrat_14), 0);
    lv_obj_align(s_storage_sub, LV_ALIGN_LEFT_MID, 62, -2);

    s_storage_bar = lv_bar_create(row);
    lv_obj_set_size(s_storage_bar, 200, 8);
    lv_obj_align(s_storage_bar, LV_ALIGN_LEFT_MID, 62, 26);
    lv_bar_set_range(s_storage_bar, 0, 100);
    lv_obj_set_style_radius(s_storage_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_storage_bar, lv_color_hex(UI_COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_storage_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_storage_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_storage_bar, lv_color_hex(UI_COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(s_storage_bar, lv_color_hex(UI_COLOR_ACCENT_2), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(s_storage_bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);

    lv_obj_t *arrow = lv_label_create(row);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(arrow, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_16, 0);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -16, 0);
}

/* 音量变化：拖动时只更新文字，松手才写 NVS（避免每移动一格都 commit） */
static void volume_changed_cb(lv_event_t *event)
{
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(event);
    const int volume = (int)lv_slider_get_value(slider);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", volume);
    lv_label_set_text(s_vol_label, buf);

    if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
        app_settings_set_speaker_volume(volume);
        ESP_LOGI(TAG, "speaker volume -> %d%%", volume);
    }
}

/**
 * 滑杆行：图标 + 标题 + 右侧百分比 + 滑杆
 *
 * 滑杆自己处理手势，行本身不响应点击（否则拖滑杆会被当成点行）。
 * 拖动时回调只更新文字，松手才写 NVS，避免拖一次刷几十次 flash。
 *
 * @return 滑杆对象，调用方保存起来用于"每次打开刷新"
 */
static lv_obj_t *make_slider_row(lv_obj_t *parent, const char *icon, const char *title,
                                 int min, int max, int value,
                                 lv_event_cb_t cb, lv_obj_t **out_value_label)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 300, 80);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_outline_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_scrollable(row, false);

    lv_obj_t *icon_label = lv_label_create(row);
    lv_label_set_text(icon_label, icon);
    lv_obj_set_style_text_color(icon_label, lv_color_hex(UI_COLOR_ACCENT_2), 0);
    lv_obj_set_style_text_font(icon_label, &lv_font_montserrat_20, 0);
    lv_obj_align(icon_label, LV_ALIGN_LEFT_MID, 18, -6);

    lv_obj_t *title_label = lv_label_create(row);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_color(title_label, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(title_label, cjk_font_or(18, &lv_font_montserrat_16), 0);
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 62, -24);

    lv_obj_t *value_label = lv_label_create(row);
    lv_label_set_text_fmt(value_label, "%d%%", value);
    lv_obj_set_style_text_color(value_label, lv_color_hex(UI_COLOR_MUTED), 0);
    lv_obj_set_style_text_font(value_label, &lv_font_montserrat_16, 0);
    lv_obj_align(value_label, LV_ALIGN_RIGHT_MID, -16, -24);

    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_size(slider, 200, 8);
    lv_obj_align(slider, LV_ALIGN_LEFT_MID, 62, 22);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);

    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(slider, lv_color_hex(UI_COLOR_ACCENT_2), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(slider, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);

    lv_obj_set_style_bg_color(slider, lv_color_hex(UI_COLOR_TEXT), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 3, LV_PART_KNOB);        /* 旋钮 8+6=14 px */

    lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, cb, LV_EVENT_RELEASED, NULL);

    if (out_value_label != NULL) {
        *out_value_label = value_label;
    }
    return slider;
}

static void make_speaker_row(lv_obj_t *parent)
{
    s_vol_slider = make_slider_row(parent, LV_SYMBOL_VOLUME_MAX, "扬声器", 0, 100,
                                   app_settings_get_speaker_volume(), volume_changed_cb,
                                   &s_vol_label);
}

/*
 * 亮度：和音量同一套交互，但拖动时要实时预览。
 *
 * 这里不能像音量那样"每个事件只更文字"——亮度是往屏幕发 DCS 命令（0x51），
 * 一次要等 SPI 总线；拖动时一帧好几个事件，全同步写会把 LVGL 循环拖住，
 * 结果就是手柄跟不上手指、只有单击（单个事件）才生效。
 * 所以拖动期间限流到 60 ms 一次，松手再补最后一次并落 NVS。
 */
static void brightness_changed_cb(lv_event_t *event)
{
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(event);
    const int percent = (int)lv_slider_get_value(slider);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", percent);
    lv_label_set_text(s_bright_label, buf);

    const bool released = (lv_event_get_code(event) == LV_EVENT_RELEASED);
    const int now = (int)lv_tick_get();

    if (!released && (now - s_last_bright_apply_ms) < BRIGHT_APPLY_MIN_MS) {
        return;
    }
    s_last_bright_apply_ms = now;
    bsp_display_brightness_set(percent);

    if (released) {
        app_settings_set_brightness(percent);
        ESP_LOGI(TAG, "brightness -> %d%%", percent);
    }
}

static void make_brightness_row(lv_obj_t *parent)
{
    s_bright_slider = make_slider_row(parent, LV_SYMBOL_EYE_OPEN, "亮度", 10, 100,
                                      app_settings_get_brightness(), brightness_changed_cb,
                                      &s_bright_label);
}

/* 每次打开设置页都按 NVS 里的最新值刷新（app 里也可能改过） */
static void refresh_volume(void)
{
    if (s_vol_slider == NULL) {
        return;
    }

    const int volume = app_settings_get_speaker_volume();
    lv_slider_set_value(s_vol_slider, volume, LV_ANIM_OFF);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", volume);
    lv_label_set_text(s_vol_label, buf);
}

static void refresh_brightness(void)
{
    if (s_bright_slider == NULL) {
        return;
    }

    const int percent = app_settings_get_brightness();
    lv_slider_set_value(s_bright_slider, percent, LV_ANIM_OFF);
    bsp_display_brightness_set(percent);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", percent);
    lv_label_set_text(s_bright_label, buf);
}

static void create_root(void)
{
    lv_obj_t *screen = lv_screen_active();

    s_root = lv_obj_create(screen);
    lv_obj_set_size(s_root, 466, 466);
    lv_obj_center(s_root);
    ui_bg_apply(s_root);
    lv_obj_set_scrollable(s_root, false);
    lv_obj_set_clickable(s_root, true);   /* 挡住下层应用列表的点击 */
    lv_obj_set_hidden(s_root, true);

    lv_obj_t *title = lv_label_create(s_root);
    lv_label_set_text(title, "设置");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(title, cjk_font_or(20, &lv_font_montserrat_20), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    /*
     * 列表比行宽留出余量：正对中心的行会放大到 125%（300 → 375），
     * 容器窄了就会被自己裁掉两边。
     */
    lv_obj_t *list = lv_obj_create(s_root);
    lv_obj_set_size(list, 400, 288);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 82);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    make_row(list, LV_SYMBOL_WIFI, "Wi-Fi", wifi_clicked_cb, &s_wifi_sub);
    make_speaker_row(list);
    make_brightness_row(list);
    make_storage_row(list);

    /* 条目建完再挂滚动效果，attach 内部会先跑一遍布局 */
    ui_scroll_fx_attach(list, &SETTINGS_FX);

    /*
     * 返回：放标题左边，只留图标。
     * 触摸区 56x56 但图标本身只有十几像素，圆屏左上角的圆弧裁不到它。
     */
    lv_obj_t *back = lv_obj_create(s_root);
    lv_obj_set_size(back, 56, 56);
    lv_obj_align(back, LV_ALIGN_TOP_MID, -104, 24);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_set_style_pad_all(back, 0, 0);
    lv_obj_set_scrollable(back, false);
    lv_obj_set_clickable(back, true);
    tap_gesture_attach(back);
    lv_obj_add_event_cb(back, back_clicked_cb, LV_EVENT_RELEASED, NULL);

    /* 左箭头在中文字库里没有字形，用 Montserrat 单独渲染 */
    lv_obj_t *back_icon = lv_label_create(back);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_icon, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(back_icon, &lv_font_montserrat_20, 0);
    lv_obj_center(back_icon);
}

void settings_open(void)
{
    if (s_root == NULL) {
        create_root();
    }
    refresh_wifi_subtitle();
    refresh_volume();
    refresh_brightness();
    refresh_storage();
    lv_obj_set_hidden(s_root, false);
    lv_obj_move_to_index(s_root, -1);
}
