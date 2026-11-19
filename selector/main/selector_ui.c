#include "selector_ui.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "lvgl.h"

#include "cjk_font.h"
#include "image_install.h"
#include "wifi_config.h"
#include "wifi_setup.h"

static const char *TAG = "selector.ui";

#define COLOR_BG        0x000000
#define COLOR_TEXT      0xF2F2F2
#define COLOR_MUTED     0x7A7A7A
#define COLOR_ROW       0x141414
#define COLOR_ROW_EDGE  0x2A2A2A
#define COLOR_OK        0x2ECC71
#define COLOR_ERR       0xE74C3C

static const image_entry_t *s_images = NULL;
static int s_count = 0;
static selector_select_cb_t s_select_cb = NULL;

static lv_obj_t *s_wifi_status = NULL;

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

static void on_row_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || s_select_cb == NULL) {
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

static void on_wifi_clicked(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (lv_tick_get() - s_created_tick < CLICK_GUARD_MS) {
        ESP_LOGI(TAG, "ignoring touch during startup guard");
        return;
    }
    wifi_setup_open();
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
    lv_obj_add_event_cb(row, on_row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)index);

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

/** 每秒刷新底部 Wi-Fi 状态；设置页保存后无需手动通知。 */
static void refresh_wifi_status(lv_timer_t *timer)
{
    (void)timer;
    static char last[160];

    wifi_cred_t cred;
    const bool has_cred = (wifi_config_load(&cred) == ESP_OK);
    const char *ssid = has_cred ? cred.ssid : NULL;
    const wifi_config_state_t state = wifi_config_get_state();

    char buf[160];
    if (state == WIFI_CFG_STATE_CONNECTED) {
        snprintf(buf, sizeof(buf), "wifi: %s 已连接 %s", ssid ? ssid : "", wifi_config_get_ip());
    } else if (state == WIFI_CFG_STATE_CONNECTING) {
        snprintf(buf, sizeof(buf), "wifi: %s 连接中…", ssid ? ssid : "");
    } else if (ssid == NULL) {
        snprintf(buf, sizeof(buf), "wifi: 未设置，点右侧按钮配置");
    } else {
        snprintf(buf, sizeof(buf), "wifi: %s 未连接", ssid);
    }

    if (s_wifi_status == NULL || strcmp(buf, last) == 0) {
        return;
    }
    strlcpy(last, buf, sizeof(last));
    lv_label_set_text(s_wifi_status, buf);
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
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(screen, false);

    /*
     * 圆屏布局约束：可视区域是 466x466 的内切圆（半径 233，圆心 233,233）。
     * 所有内容控制在半径 200 以内，否则会被物理圆边裁掉、且触摸点不到。
     *   y=40  处可用半宽约 155
     *   y=100 处约 205
     *   y=320 处约 219
     *   y=400 处约 152
     */
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "APPS");
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_set_style_text_font(title, cjk_font_or(20, &lv_font_montserrat_20), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

    lv_obj_t *list = lv_obj_create(screen);
    lv_obj_set_size(list, 320, 250);
    lv_obj_align(list, LV_ALIGN_CENTER, 0, -40);
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

    /* Wi-Fi 设置入口：底部安全区内，整条 150x42 都可点 */
    lv_obj_t *wifi_btn = lv_obj_create(screen);
    lv_obj_set_size(wifi_btn, 150, 42);
    lv_obj_align(wifi_btn, LV_ALIGN_BOTTOM_MID, 0, -100);
    lv_obj_set_style_bg_color(wifi_btn, lv_color_hex(COLOR_ROW), 0);
    lv_obj_set_style_bg_opa(wifi_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(wifi_btn, 21, 0);
    lv_obj_set_style_border_width(wifi_btn, 1, 0);
    lv_obj_set_style_border_color(wifi_btn, lv_color_hex(COLOR_ROW_EDGE), 0);
    lv_obj_set_style_pad_all(wifi_btn, 0, 0);
    lv_obj_set_scrollable(wifi_btn, false);
    lv_obj_set_clickable(wifi_btn, true);
    lv_obj_add_event_cb(wifi_btn, on_wifi_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *wifi_label = lv_label_create(wifi_btn);
    lv_label_set_text(wifi_label, "WIFI SETUP");
    lv_obj_set_style_text_color(wifi_label, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(wifi_label, cjk_font_or(14, &lv_font_montserrat_14), 0);
    lv_obj_center(wifi_label);

    lv_obj_t *footer = lv_label_create(screen);
    lv_label_set_text_fmt(
        footer, "%d %s | %.1f / %.1f MB used",
        count,
        count == 1 ? "image" : "images",
        (double)fs_used / (1024.0 * 1024.0),
        (double)(fs_total / (1024.0 * 1024.0))
    );
    lv_obj_set_style_text_color(footer, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_font(footer, cjk_font_or(14, &lv_font_montserrat_14), 0);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -58);

    s_wifi_status = lv_label_create(screen);
    lv_label_set_text(s_wifi_status, "");
    lv_obj_set_style_text_color(s_wifi_status, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_set_style_text_font(s_wifi_status, cjk_font_or(14, &lv_font_montserrat_14), 0);
    lv_obj_align(s_wifi_status, LV_ALIGN_BOTTOM_MID, 0, -34);
    refresh_wifi_status(NULL);
    lv_timer_create(refresh_wifi_status, 1000, NULL);

    create_overlay(screen);

    s_created_tick = lv_tick_get();
    lv_timer_create(poll_timer_cb, 100, NULL);
}
