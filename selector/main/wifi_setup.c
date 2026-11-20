/*
 * Wi-Fi 设置页：扫描 → 选 AP → 键盘输密码 → 写 NVS
 *
 * 全程运行在 LVGL 任务上下文（调用方已持锁），内部不再加锁。
 */
#include "wifi_setup.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "cjk_font.h"
#include "tap_gesture.h"
#include "ui_theme.h"
#include "wifi_config.h"

static const char *TAG = "selector.wifi";

#define COLOR_BG        0x000000
#define COLOR_TEXT      0xF2F2F2
#define COLOR_MUTED     0x7A7A7A
#define COLOR_ROW       0x141414
#define COLOR_ROW_EDGE  0x2A2A2A
#define COLOR_OK        0x2ECC71
#define COLOR_ACCENT    0x3498DB

#define MAX_APS         8
#define AP_LIST_W       360
#define AP_LIST_H       230

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_status = NULL;
static lv_obj_t *s_list = NULL;
static lv_obj_t *s_pw_panel = NULL;
static lv_obj_t *s_pw_title = NULL;
static lv_obj_t *s_ta = NULL;
static lv_obj_t *s_kb = NULL;

static wifi_ap_t s_aps[MAX_APS];
static int s_ap_count = 0;

static volatile bool s_scanning = false;
static volatile bool s_scan_done = false;
static char s_selected_ssid[WIFI_CRED_SSID_MAX];

/* -------------------------------------------------------------------------- */

static void update_status_text(void)
{
    if (s_status == NULL) {
        return;
    }

    if (s_scanning) {
        lv_label_set_text(s_status, "scanning...");
        lv_obj_set_style_text_color(s_status, lv_color_hex(COLOR_ACCENT), 0);
        return;
    }

    wifi_cred_t cred;
    if (wifi_config_load(&cred) == ESP_OK) {
        lv_label_set_text_fmt(s_status, "saved: %s", cred.ssid);
        lv_obj_set_style_text_color(s_status, lv_color_hex(COLOR_OK), 0);
    } else {
        lv_label_set_text(s_status, "not configured");
        lv_obj_set_style_text_color(s_status, lv_color_hex(COLOR_MUTED), 0);
    }
}

static void scan_task(void *arg)
{
    (void)arg;

    wifi_ap_t found[MAX_APS];
    int count = 0;
    esp_err_t err = wifi_config_scan(found, MAX_APS, &count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        count = 0;
    } else {
        memcpy(s_aps, found, sizeof(found));
    }

    s_ap_count = count;
    s_scanning = false;
    s_scan_done = true;
    vTaskDelete(NULL);
}

static void start_scan(void)
{
    if (s_scanning) {
        return;
    }
    s_scanning = true;
    s_scan_done = false;
    update_status_text();

    if (xTaskCreate(scan_task, "wifi_scan", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create scan task");
        s_scanning = false;
        update_status_text();
    }
}

/* -------------------------------------------------------------------------- */
/* 密码输入                                                                   */

static void hide_password_panel(void)
{
    if (s_pw_panel != NULL) {
        lv_obj_set_hidden(s_pw_panel, true);
    }
    if (s_list != NULL) {
        lv_obj_set_hidden(s_list, false);
    }
}

static void keyboard_event_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_READY) {
        const char *text = lv_textarea_get_text(s_ta);
        esp_err_t err = wifi_config_save(s_selected_ssid, text != NULL ? text : "");
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "saved credentials for \"%s\"", s_selected_ssid);
        } else {
            ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
        }
        hide_password_panel();
        s_scanning = false;
        update_status_text();
    } else if (code == LV_EVENT_CANCEL) {
        hide_password_panel();
    }
}

static void show_password_panel(const char *ssid)
{
    strlcpy(s_selected_ssid, ssid, sizeof(s_selected_ssid));

    if (s_list != NULL) {
        lv_obj_set_hidden(s_list, true);
    }
    if (s_pw_panel == NULL) {
        return;
    }

    lv_label_set_text_fmt(s_pw_title, "password for %s", s_selected_ssid);
    lv_textarea_set_text(s_ta, "");
    lv_obj_set_hidden(s_pw_panel, false);
    lv_obj_move_to_index(s_pw_panel, -1);
}

/* -------------------------------------------------------------------------- */
/* AP 列表                                                                    */

static void ap_clicked_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_RELEASED) {
        return;
    }
    if (!tap_gesture_was_tap()) {
        return;
    }
    const int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (index < 0 || index >= s_ap_count) {
        return;
    }
    show_password_panel(s_aps[index].ssid);
}

static void rebuild_list(void)
{
    if (s_list == NULL) {
        return;
    }
    lv_obj_clean(s_list);

    if (s_ap_count == 0) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_label_set_text(empty, "no network found\ntap RESCAN");
        lv_obj_set_style_text_color(empty, lv_color_hex(COLOR_MUTED), 0);
        lv_obj_set_style_text_font(empty, cjk_font_or(14, &lv_font_montserrat_14), 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    for (int i = 0; i < s_ap_count; i++) {
        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_set_size(row, AP_LIST_W - 16, 46);
        lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_ROW), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(COLOR_ROW_EDGE), 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_scrollable(row, false);
        lv_obj_set_clickable(row, true);
        tap_gesture_attach(row);
        lv_obj_add_event_cb(row, ap_clicked_cb, LV_EVENT_RELEASED, (void *)(intptr_t)i);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text_fmt(name, "%s", s_aps[i].ssid);
        lv_obj_set_style_text_color(name, lv_color_hex(COLOR_TEXT), 0);
        lv_obj_set_style_text_font(name, cjk_font_or(14, &lv_font_montserrat_14), 0);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 14, 0);

        lv_obj_t *meta = lv_label_create(row);
        lv_label_set_text_fmt(meta, "%d%s", (int)s_aps[i].rssi, s_aps[i].secure ? " L" : " O");
        lv_obj_set_style_text_color(meta, lv_color_hex(COLOR_MUTED), 0);
        lv_obj_set_style_text_font(meta, cjk_font_or(14, &lv_font_montserrat_14), 0);
        lv_obj_align(meta, LV_ALIGN_RIGHT_MID, -14, 0);
    }
}

/* -------------------------------------------------------------------------- */
/* 页面                                                                       */

static void poll_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_scan_done) {
        return;
    }
    s_scan_done = false;
    update_status_text();
    rebuild_list();
}

static void rescan_clicked_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_RELEASED || !tap_gesture_was_tap()) {
        return;
    }
    start_scan();
}

static void close_clicked_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_RELEASED || !tap_gesture_was_tap()) {
        return;
    }
    hide_password_panel();
    lv_obj_set_hidden(s_root, true);
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, int x, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, 110, 42);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COLOR_ROW), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 21, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(COLOR_ROW_EDGE), 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_scrollable(btn, false);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, x, -44);   /* 圆屏下沿要留出弧度 */
    tap_gesture_attach(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(label, cjk_font_or(14, &lv_font_montserrat_14), 0);
    lv_obj_center(label);
    return btn;
}

void wifi_setup_open(void)
{
    if (s_root != NULL) {
        lv_obj_set_hidden(s_root, false);
        lv_obj_move_to_index(s_root, -1);
        return;
    }

    lv_obj_t *screen = lv_screen_active();

    s_root = lv_obj_create(screen);
    lv_obj_set_size(s_root, 466, 466);
    lv_obj_center(s_root);
    ui_bg_apply(s_root);
    lv_obj_set_scrollable(s_root, false);
    lv_obj_set_clickable(s_root, true);   /* 挡住下层列表的点击 */

    lv_obj_t *title = lv_label_create(s_root);
    lv_label_set_text(title, "WIFI SETUP");
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(title, cjk_font_or(20, &lv_font_montserrat_20), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 48);

    s_status = lv_label_create(s_root);
    lv_label_set_text(s_status, "");
    lv_obj_set_style_text_color(s_status, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_set_style_text_font(s_status, cjk_font_or(14, &lv_font_montserrat_14), 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 82);

    s_list = lv_obj_create(s_root);
    lv_obj_set_size(s_list, AP_LIST_W, AP_LIST_H);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, 6, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    make_button(s_root, "RESCAN", -60, rescan_clicked_cb);
    make_button(s_root, "CLOSE", 60, close_clicked_cb);

    /* 密码输入层：默认隐藏，选中 AP 后弹出键盘 */
    s_pw_panel = lv_obj_create(s_root);
    lv_obj_set_size(s_pw_panel, 466, 466);
    lv_obj_center(s_pw_panel);
    ui_bg_apply(s_pw_panel);
    lv_obj_set_scrollable(s_pw_panel, false);
    lv_obj_set_hidden(s_pw_panel, true);

    s_pw_title = lv_label_create(s_pw_panel);
    lv_label_set_text(s_pw_title, "password");
    lv_obj_set_style_text_color(s_pw_title, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_pw_title, cjk_font_or(14, &lv_font_montserrat_14), 0);
    lv_obj_align(s_pw_title, LV_ALIGN_TOP_MID, 0, 40);

    s_ta = lv_textarea_create(s_pw_panel);
    lv_textarea_set_one_line(s_ta, true);
    lv_obj_set_size(s_ta, 300, 52);
    lv_obj_align(s_ta, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_set_style_text_font(s_ta, cjk_font_or(16, &lv_font_montserrat_16), 0);

    /*
     * 键盘必须整体落在圆内：336x186 居中偏下 40px
     *   占位 y=180..366, x=65..401，最远角到圆心 214 < 233
     * 之前的 440x210 贴底摆放，左下角的 "1#" 和右下角的回车都落在圆外，点不到。
     */
    s_kb = lv_keyboard_create(s_pw_panel);
    lv_obj_set_size(s_kb, 336, 186);
    lv_obj_align(s_kb, LV_ALIGN_CENTER, 0, 40);
    lv_obj_set_style_bg_color(s_kb, lv_color_hex(COLOR_ROW), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_kb, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_kb, lv_color_hex(0x2A2A2A), LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_kb, lv_color_hex(COLOR_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_kb, 0, LV_PART_MAIN);
    lv_keyboard_set_textarea(s_kb, s_ta);
    lv_obj_add_event_cb(s_kb, keyboard_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_kb, keyboard_event_cb, LV_EVENT_CANCEL, NULL);

    lv_timer_create(poll_timer_cb, 200, NULL);

    update_status_text();
    start_scan();
    ESP_LOGI(TAG, "wifi setup opened");
}
