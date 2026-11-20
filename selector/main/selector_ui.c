#include "selector_ui.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "lvgl.h"

#include "battery.h"
#include "cjk_font.h"
#include "clock_sync.h"
#include "image_install.h"
#include "settings_ui.h"
#include "tap_gesture.h"
#include "ui_theme.h"
#include "wifi_config.h"

static const char *TAG = "selector.ui";

/* 配色统一在 ui_theme.h，这里用短名字指代，避免改动过多老代码 */
#define COLOR_BG        UI_COLOR_BG_TOP
#define COLOR_TEXT      UI_COLOR_TEXT
#define COLOR_MUTED     UI_COLOR_MUTED
#define COLOR_ROW       UI_COLOR_ROW
#define COLOR_ROW_EDGE  UI_COLOR_ROW_EDGE
#define COLOR_OK        UI_COLOR_OK
#define COLOR_ERR       UI_COLOR_ERR

static const image_entry_t *s_images = NULL;
static int s_count = 0;
static selector_select_cb_t s_select_cb = NULL;

/* 顶部状态栏：Wi-Fi / 时间 / 电量 */
static lv_obj_t *s_wifi_icon = NULL;
static lv_obj_t *s_clock_label = NULL;
static lv_obj_t *s_bat_charge = NULL;
static lv_obj_t *s_bat_icon = NULL;
static lv_obj_t *s_bat_text = NULL;

static lv_obj_t *s_overlay = NULL;
static lv_obj_t *s_overlay_title = NULL;
static lv_obj_t *s_overlay_msg = NULL;
static lv_obj_t *s_bar = NULL;
static lv_obj_t *s_bar_value = NULL;

static int s_done_ticks = 0;
static int s_failed_ticks = 0;
static uint32_t s_created_tick = 0;

/* 触摸控制器上电时可能给出一次假触摸，界面刚出现的这段时间忽略点击 */
#define CLICK_GUARD_MS  800

/* -------------------------------------------------------------------------- */

static void overlay_show(bool visible)
{
    if (s_overlay == NULL) {
        return;
    }
    if (visible) {
        lv_obj_set_hidden(s_overlay, false);
        lv_obj_move_to_index(s_overlay, -1);
    } else {
        lv_obj_set_hidden(s_overlay, true);
    }
}

static void on_row_released(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_RELEASED || s_select_cb == NULL) {
        return;
    }
    if (!tap_gesture_was_tap()) {
        return;
    }

    if (lv_tick_get() - s_created_tick < CLICK_GUARD_MS) {
        ESP_LOGI(TAG, "ignoring touch during startup guard");
        return;
    }

    const int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (index < 0 || index >= s_count) {
        return;
    }

    image_install_status_t status;
    image_install_status(&status);
    if (status.state == IMAGE_INSTALL_RUNNING || status.state == IMAGE_INSTALL_DONE) {
        return;
    }

    ESP_LOGI(TAG, "selected %s", s_images[index].file);
    s_select_cb(s_images[index].file);
}

static void on_overlay_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    image_install_status_t status;
    image_install_status(&status);
    if (status.state == IMAGE_INSTALL_FAILED) {
        image_install_reset();
        s_failed_ticks = 0;
        overlay_show(false);
    }
}

static void on_settings_released(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_RELEASED) {
        return;
    }
    if (!tap_gesture_was_tap()) {
        return;
    }
    if (lv_tick_get() - s_created_tick < CLICK_GUARD_MS) {
        ESP_LOGI(TAG, "ignoring touch during startup guard");
        return;
    }
    settings_open();
}

/* -------------------------------------------------------------------------- */

static lv_obj_t *create_row(lv_obj_t *parent, const image_entry_t *entry, int index)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 300, 56);
    lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_ROW), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 14, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(COLOR_ROW_EDGE), 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_clickable(row, true);
    tap_gesture_attach(row);
    lv_obj_add_event_cb(row, on_row_released, LV_EVENT_RELEASED, (void *)(intptr_t)index);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, entry->name);
    lv_obj_set_style_text_color(name, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(name, cjk_font_or(16, &lv_font_montserrat_16), 0);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 16, -8);

    lv_obj_t *version = lv_label_create(row);
    lv_label_set_text_fmt(version, "v%s  %u KB", entry->version, (unsigned)(entry->size / 1024));
    lv_obj_set_style_text_color(version, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_set_style_text_font(version, cjk_font_or(14, &lv_font_montserrat_14), 0);
    lv_obj_align(version, LV_ALIGN_LEFT_MID, 16, 12);

    return row;
}

/* 设置入口：和应用行同样大小，排在列表末尾，和 app 平级 */
static void create_settings_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 300, 56);
    lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_ROW), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_80, 0);
    lv_obj_set_style_radius(row, 14, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(COLOR_ROW_EDGE), 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_clickable(row, true);
    tap_gesture_attach(row);
    lv_obj_add_event_cb(row, on_settings_released, LV_EVENT_RELEASED, NULL);

    /* 图标只能用 Montserrat：CJK 字体里没有 LV_SYMBOL_* 的字形 */
    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text(icon, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(icon, lv_color_hex(UI_COLOR_ACCENT_2), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 18, 0);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, "设置");
    lv_obj_set_style_text_color(name, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(name, cjk_font_or(16, &lv_font_montserrat_16), 0);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 56, -8);

    lv_obj_t *sub = lv_label_create(row);
    lv_label_set_text(sub, "Wi-Fi / 时间 / 存储");
    lv_obj_set_style_text_color(sub, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_set_style_text_font(sub, cjk_font_or(14, &lv_font_montserrat_14), 0);
    lv_obj_align(sub, LV_ALIGN_LEFT_MID, 56, 12);

    lv_obj_t *arrow = lv_label_create(row);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(arrow, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_16, 0);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -14, 0);
}

/*
 * 顶部状态栏：Wi-Fi 图标 | 时间 | 电量
 * 宽度 240 是按圆屏 y≈32 处的可用弦宽取的（上下都留了边）。
 */
static void create_status_bar(lv_obj_t *screen)
{
    lv_obj_t *bar = lv_obj_create(screen);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 240, 26);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_set_scrollable(bar, false);

    /*
     * 不用 flex 分配宽度：时间单独居中（相对状态栏居中 = 相对屏幕居中），
     * 两侧图标各自贴边，时间就不会因为左右内容宽度不同而偏。
     */
    s_clock_label = lv_label_create(bar);
    lv_label_set_text(s_clock_label, "--:--");
    lv_obj_set_style_text_color(s_clock_label, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_clock_label, &lv_font_montserrat_20, 0);
    lv_obj_align(s_clock_label, LV_ALIGN_CENTER, 0, 0);

    s_wifi_icon = lv_label_create(bar);
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_wifi_icon, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(s_wifi_icon, &lv_font_montserrat_20, 0);
    /* 离左边缘留一点距离，避免贴到圆屏弧线 */
    lv_obj_align(s_wifi_icon, LV_ALIGN_LEFT_MID, 16, 0);

    lv_obj_t *bat_box = lv_obj_create(bar);
    lv_obj_remove_style_all(bat_box);
    lv_obj_set_size(bat_box, LV_SIZE_CONTENT, 26);
    lv_obj_align(bat_box, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_flex_flow(bat_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bat_box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bat_box, 4, 0);
    lv_obj_set_scrollable(bat_box, false);

    s_bat_charge = lv_label_create(bat_box);
    lv_label_set_text(s_bat_charge, "");
    lv_obj_set_style_text_color(s_bat_charge, lv_color_hex(UI_COLOR_WARN), 0);
    lv_obj_set_style_text_font(s_bat_charge, &lv_font_montserrat_16, 0);

    s_bat_icon = lv_label_create(bat_box);
    lv_label_set_text(s_bat_icon, LV_SYMBOL_BATTERY_EMPTY);
    lv_obj_set_style_text_color(s_bat_icon, lv_color_hex(UI_COLOR_DIM), 0);
    lv_obj_set_style_text_font(s_bat_icon, &lv_font_montserrat_20, 0);

    s_bat_text = lv_label_create(bat_box);
    lv_label_set_text(s_bat_text, "");
    lv_obj_set_style_text_color(s_bat_text, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_set_style_text_font(s_bat_text, &lv_font_montserrat_14, 0);
}

static void create_overlay(lv_obj_t *screen)
{
    s_overlay = lv_obj_create(screen);
    lv_obj_set_size(s_overlay, 466, 466);
    lv_obj_align(s_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_set_scrollable(s_overlay, false);
    lv_obj_set_clickable(s_overlay, true);
    lv_obj_add_event_cb(s_overlay, on_overlay_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_set_hidden(s_overlay, true);

    s_overlay_title = lv_label_create(s_overlay);
    lv_label_set_text(s_overlay_title, "");
    lv_obj_set_style_text_color(s_overlay_title, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_overlay_title, cjk_font_or(20, &lv_font_montserrat_20), 0);
    lv_obj_align(s_overlay_title, LV_ALIGN_CENTER, 0, -56);

    s_bar = lv_bar_create(s_overlay);
    lv_obj_set_size(s_bar, 260, 12);
    lv_obj_align(s_bar, LV_ALIGN_CENTER, 0, 0);
    lv_bar_set_range(s_bar, 0, 100);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x1E1E1E), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(COLOR_OK), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 6, LV_PART_INDICATOR);

    s_bar_value = lv_label_create(s_overlay);
    lv_label_set_text(s_bar_value, "");
    lv_obj_set_style_text_color(s_bar_value, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_set_style_text_font(s_bar_value, cjk_font_or(14, &lv_font_montserrat_14), 0);
    lv_obj_align(s_bar_value, LV_ALIGN_CENTER, 0, 30);

    s_overlay_msg = lv_label_create(s_overlay);
    lv_label_set_text(s_overlay_msg, "");
    lv_obj_set_style_text_color(s_overlay_msg, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_set_style_text_font(s_overlay_msg, cjk_font_or(14, &lv_font_montserrat_14), 0);
    lv_obj_set_style_text_align(s_overlay_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_overlay_msg, LV_ALIGN_CENTER, 0, 72);
}

/* -------------------------------------------------------------------------- */

static void poll_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    image_install_status_t status;
    image_install_status(&status);
    if (status.state == IMAGE_INSTALL_IDLE) {
        return;
    }

    overlay_show(true);

    switch (status.state) {
    case IMAGE_INSTALL_RUNNING:
        lv_bar_set_value(s_bar, status.percent, LV_ANIM_OFF);
        lv_label_set_text_fmt(s_bar_value, "%d%%", status.percent);
        lv_label_set_text(s_overlay_title, "INSTALLING");
        lv_obj_set_style_text_color(s_overlay_title, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_style_bg_color(s_bar, lv_color_hex(COLOR_OK), LV_PART_INDICATOR);
        lv_label_set_text(s_overlay_msg, status.message);
        break;

    case IMAGE_INSTALL_DONE:
        lv_bar_set_value(s_bar, 100, LV_ANIM_OFF);
        lv_label_set_text(s_bar_value, "100%");
        lv_label_set_text(s_overlay_title, "INSTALLED");
        lv_obj_set_style_text_color(s_overlay_title, lv_color_hex(COLOR_OK), 0);
        lv_label_set_text(s_overlay_msg, status.message);
        if (++s_done_ticks >= 8) {
            esp_restart();
        }
        break;

    case IMAGE_INSTALL_FAILED:
        lv_label_set_text(s_overlay_title, "INSTALL FAILED");
        lv_obj_set_style_text_color(s_overlay_title, lv_color_hex(COLOR_ERR), 0);
        lv_obj_set_style_bg_color(s_bar, lv_color_hex(COLOR_ERR), LV_PART_INDICATOR);
        lv_label_set_text(s_overlay_msg, status.message);
        if (++s_failed_ticks >= 50) {
            image_install_reset();
            s_failed_ticks = 0;
            overlay_show(false);
        }
        break;

    default:
        break;
    }
}

/* -------------------------------------------------------------------------- */

static const char *battery_symbol(uint8_t percent)
{
    if (percent >= 90) {
        return LV_SYMBOL_BATTERY_FULL;
    }
    if (percent >= 70) {
        return LV_SYMBOL_BATTERY_3;
    }
    if (percent >= 45) {
        return LV_SYMBOL_BATTERY_2;
    }
    if (percent >= 20) {
        return LV_SYMBOL_BATTERY_1;
    }
    return LV_SYMBOL_BATTERY_EMPTY;
}

/** 每秒刷新时间与 Wi-Fi 图标；电量 30 s 一次，避免频繁占用 I2C */
static void status_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    static int battery_ticks = 0;

    char hhmm[8];
    clock_sync_hhmm(hhmm, sizeof(hhmm));
    if (s_clock_label != NULL && strcmp(lv_label_get_text(s_clock_label), hhmm) != 0) {
        lv_label_set_text(s_clock_label, hhmm);
    }

    if (s_wifi_icon != NULL) {
        const wifi_config_state_t state = wifi_config_get_state();
        uint32_t color = UI_COLOR_DIM;
        if (state == WIFI_CFG_STATE_CONNECTED) {
            color = UI_COLOR_OK;
        } else if (state == WIFI_CFG_STATE_CONNECTING) {
            color = UI_COLOR_WARN;
        }
        lv_obj_set_style_text_color(s_wifi_icon, lv_color_hex(color), 0);
    }

    if (battery_ticks-- > 0) {
        return;
    }
    battery_ticks = 30;

    battery_info_t info;
    if (battery_read(&info) != ESP_OK) {
        return;
    }

    if (!info.present) {
        lv_label_set_text(s_bat_icon, LV_SYMBOL_BATTERY_EMPTY);
        lv_obj_set_style_text_color(s_bat_icon, lv_color_hex(UI_COLOR_DIM), 0);
        lv_label_set_text(s_bat_charge, "");
        lv_label_set_text(s_bat_text, "");
        return;
    }

    /* 图标配色：充电黄 / 满电绿 / 其余白；百分比文字固定灰色 */
    uint32_t icon_color = UI_COLOR_TEXT;
    if (info.charging) {
        icon_color = UI_COLOR_WARN;
    } else if (info.percent >= 100) {
        icon_color = UI_COLOR_OK;
    }

    lv_label_set_text(s_bat_icon, battery_symbol(info.percent));
    lv_obj_set_style_text_color(s_bat_icon, lv_color_hex(icon_color), 0);
    lv_label_set_text(s_bat_charge, info.charging ? LV_SYMBOL_CHARGE : "");

    char pct_text[8];
    snprintf(pct_text, sizeof(pct_text), "%u%%", (unsigned)info.percent);
    lv_label_set_text(s_bat_text, pct_text);

    ESP_LOGI(TAG, "battery %u%% %u mV charging=%d",
             (unsigned)info.percent, (unsigned)info.millivolt, (int)info.charging);
}

void selector_ui_create(
    const image_entry_t *images,
    int count,
    size_t fs_total,
    size_t fs_used,
    selector_select_cb_t cb
)
{
    s_images = images;
    s_count = count;
    s_select_cb = cb;

    lv_obj_t *screen = lv_screen_active();
    ui_apply_gradient(screen);
    lv_obj_set_scrollable(screen, false);

    /*
     * 圆屏布局约束：可视区域是 466x466 的内切圆（半径 233，圆心 233,233）。
     * 所有内容控制在半径 200 以内，否则会被物理圆边裁掉、且触摸点不到。
     *   y=40  处可用半宽约 155
     *   y=100 处约 205
     *   y=320 处约 219
     *   y=400 处约 152
     */
    create_status_bar(screen);

    /* 列表：3 行 app + 分隔线 + 设置行，超出就上下滑 */
    lv_obj_t *list = lv_obj_create(screen);
    lv_obj_set_size(list, 320, 250);
    lv_obj_align(list, LV_ALIGN_CENTER, 0, -38);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    if (count == 0) {
        lv_obj_t *empty = lv_label_create(list);
        lv_label_set_text(empty, "IMAGE LIBRARY EMPTY\n\npack with tools/pack_storage.ps1\nthen flash storage");
        lv_obj_set_style_text_color(empty, lv_color_hex(COLOR_MUTED), 0);
        lv_obj_set_style_text_font(empty, cjk_font_or(14, &lv_font_montserrat_14), 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
    } else {
        for (int i = 0; i < count; i++) {
            create_row(list, &images[i], i);
        }
    }

    /* 设置入口和应用行同级，中间加一条细分隔线 */
    lv_obj_t *divider = lv_obj_create(list);
    lv_obj_set_size(divider, 260, 1);
    lv_obj_set_style_bg_color(divider, lv_color_hex(COLOR_ROW_EDGE), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(divider, 0, 0);
    lv_obj_set_style_radius(divider, 0, 0);
    lv_obj_set_style_pad_all(divider, 0, 0);
    lv_obj_set_scrollable(divider, false);
    create_settings_row(list);

    /* 存储用量改到设置页展示，这里只留日志方便排查 */
    ESP_LOGI(TAG, "storage %u/%u bytes", (unsigned)fs_used, (unsigned)fs_total);

    create_overlay(screen);

    s_created_tick = lv_tick_get();
    lv_timer_create(poll_timer_cb, 100, NULL);
    lv_timer_create(status_timer_cb, 1000, NULL);
    status_timer_cb(NULL);   /* 首帧就带上时间/电量，不用等 1 s */
}
