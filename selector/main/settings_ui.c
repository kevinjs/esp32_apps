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

#include "app_settings.h"
#include "cjk_font.h"
#include "image_store.h"
#include "tap_gesture.h"
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
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_ROW), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_80, 0);
    lv_obj_set_style_radius(row, 16, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(UI_COLOR_ROW_EDGE), 0);
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

/* 存储行：图标 + 标题 + 用量文字 + 进度条（不可点，纯展示） */
static void make_storage_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 300, 88);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_ROW), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_80, 0);
    lv_obj_set_style_radius(row, 16, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(UI_COLOR_ROW_EDGE), 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_scrollable(row, false);

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

/* 扬声器行：图标 + 标题 + 百分比 + 滑杆（滑杆自己处理手势，行本身不响应点击） */
static void make_speaker_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 300, 80);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_ROW), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_80, 0);
    lv_obj_set_style_radius(row, 16, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(UI_COLOR_ROW_EDGE), 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_scrollable(row, false);

    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text(icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(icon, lv_color_hex(UI_COLOR_ACCENT_2), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 18, -6);

    lv_obj_t *title = lv_label_create(row);
    lv_label_set_text(title, "扬声器");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(title, cjk_font_or(18, &lv_font_montserrat_16), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 62, -24);

    s_vol_label = lv_label_create(row);
    lv_label_set_text(s_vol_label, "");
    lv_obj_set_style_text_color(s_vol_label, lv_color_hex(UI_COLOR_MUTED), 0);
    lv_obj_set_style_text_font(s_vol_label, &lv_font_montserrat_16, 0);
    lv_obj_align(s_vol_label, LV_ALIGN_RIGHT_MID, -16, -24);

    s_vol_slider = lv_slider_create(row);
    lv_obj_set_size(s_vol_slider, 200, 8);
    lv_obj_align(s_vol_slider, LV_ALIGN_LEFT_MID, 62, 22);
    lv_slider_set_range(s_vol_slider, 0, 100);
    lv_slider_set_value(s_vol_slider, app_settings_get_speaker_volume(), LV_ANIM_OFF);

    lv_obj_set_style_radius(s_vol_slider, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_vol_slider, lv_color_hex(UI_COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_vol_slider, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_set_style_radius(s_vol_slider, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_vol_slider, lv_color_hex(UI_COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(s_vol_slider, lv_color_hex(UI_COLOR_ACCENT_2), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(s_vol_slider, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);

    lv_obj_set_style_bg_color(s_vol_slider, lv_color_hex(UI_COLOR_TEXT), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_vol_slider, 3, LV_PART_KNOB);   /* 旋钮 8+6=14 px */

    lv_obj_add_event_cb(s_vol_slider, volume_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_vol_slider, volume_changed_cb, LV_EVENT_RELEASED, NULL);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", app_settings_get_speaker_volume());
    lv_label_set_text(s_vol_label, buf);
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

static void create_root(void)
{
    lv_obj_t *screen = lv_screen_active();

    s_root = lv_obj_create(screen);
    lv_obj_set_size(s_root, 466, 466);
    lv_obj_center(s_root);
    ui_apply_gradient(s_root);
    lv_obj_set_scrollable(s_root, false);
    lv_obj_set_clickable(s_root, true);   /* 挡住下层应用列表的点击 */
    lv_obj_set_hidden(s_root, true);

    lv_obj_t *title = lv_label_create(s_root);
    lv_label_set_text(title, "设置");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(title, cjk_font_or(20, &lv_font_montserrat_20), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *list = lv_obj_create(s_root);
    lv_obj_set_size(list, 320, 256);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 90);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    make_row(list, LV_SYMBOL_WIFI, "Wi-Fi", wifi_clicked_cb, &s_wifi_sub);
    make_speaker_row(list);
    make_storage_row(list);

    /*
     * 返回按钮放底部：圆屏左上角在 y≈30 处可用宽度只有 ~220，
     * 44x44 的按钮会被圆弧裁掉一半，底部才是能点到的安全区。
     */
    lv_obj_t *back = lv_obj_create(s_root);
    lv_obj_set_size(back, 140, 44);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -46);
    lv_obj_set_style_radius(back, 22, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(UI_COLOR_ROW), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_80, 0);
    lv_obj_set_style_border_width(back, 1, 0);
    lv_obj_set_style_border_color(back, lv_color_hex(UI_COLOR_ROW_EDGE), 0);
    lv_obj_set_style_pad_all(back, 0, 0);
    lv_obj_set_scrollable(back, false);
    lv_obj_set_clickable(back, true);
    tap_gesture_attach(back);
    lv_obj_add_event_cb(back, back_clicked_cb, LV_EVENT_RELEASED, NULL);

    /* 箭头在中文字库里没有字形，必须拆成两个 label */
    lv_obj_t *back_arrow = lv_label_create(back);
    lv_label_set_text(back_arrow, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_arrow, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(back_arrow, &lv_font_montserrat_16, 0);
    lv_obj_align(back_arrow, LV_ALIGN_LEFT_MID, 30, 0);

    lv_obj_t *back_label = lv_label_create(back);
    lv_label_set_text(back_label, "返回");
    lv_obj_set_style_text_color(back_label, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(back_label, cjk_font_or(16, &lv_font_montserrat_16), 0);
    lv_obj_align(back_label, LV_ALIGN_CENTER, 10, 0);
}

void settings_open(void)
{
    if (s_root == NULL) {
        create_root();
    }
    refresh_wifi_subtitle();
    refresh_volume();
    refresh_storage();
    lv_obj_set_hidden(s_root, false);
    lv_obj_move_to_index(s_root, -1);
}
