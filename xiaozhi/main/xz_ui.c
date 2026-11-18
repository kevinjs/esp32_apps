#include "xz_ui.h"

#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "xz.ui";

#define COL_BG      0x000000
#define COL_TEXT    0xFFFFFF
#define COL_MUTED   0x808080
#define COL_ACCENT  0x2ECC71
#define COL_LISTEN  0x3498DB

#define UI_LOCK_MS  1000

static lv_obj_t *s_state = NULL;
static lv_obj_t *s_detail = NULL;
static lv_obj_t *s_code = NULL;
static lv_obj_t *s_button = NULL;
static xz_ui_click_cb_t s_click_cb = NULL;

/* -------------------------------------------------------------------------- */

static void button_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || s_click_cb == NULL) {
        return;
    }
    s_click_cb();
}

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

/* -------------------------------------------------------------------------- */

esp_err_t xz_ui_start(xz_ui_click_cb_t cb)
{
    s_click_cb = cb;

    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        ESP_LOGE(TAG, "bsp_display_start failed");
        return ESP_FAIL;
    }

    if (bsp_display_lock(UI_LOCK_MS) != ESP_OK) {
        ESP_LOGE(TAG, "failed to lock LVGL");
        return ESP_FAIL;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_scroll_dir(scr, LV_DIR_NONE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_state = lv_label_create(scr);
    lv_label_set_text(s_state, "BOOTING");
    lv_obj_set_style_text_color(s_state, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(s_state, &lv_font_montserrat_28, 0);
    lv_obj_align(s_state, LV_ALIGN_TOP_MID, 0, 96);

    s_code = lv_label_create(scr);
    lv_label_set_text(s_code, "");
    lv_obj_set_style_text_color(s_code, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(s_code, &lv_font_montserrat_28, 0);
    lv_obj_align(s_code, LV_ALIGN_CENTER, 0, -40);

    s_detail = lv_label_create(scr);
    lv_label_set_text(s_detail, "");
    lv_obj_set_style_text_color(s_detail, lv_color_hex(COL_MUTED), 0);
    lv_obj_set_style_text_font(s_detail, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_detail, 320);
    lv_obj_align(s_detail, LV_ALIGN_CENTER, 0, 60);

    s_button = lv_obj_create(scr);
    lv_obj_set_size(s_button, 88, 88);
    lv_obj_set_style_radius(s_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_button, lv_color_hex(COL_LISTEN), 0);
    lv_obj_set_style_bg_opa(s_button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_button, 0, 0);
    lv_obj_align(s_button, LV_ALIGN_BOTTOM_MID, 0, -56);
    lv_obj_add_event_cb(s_button, button_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_label = lv_label_create(s_button);
    lv_label_set_text(btn_label, "TALK");
    lv_obj_set_style_text_color(btn_label, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(btn_label, &lv_font_montserrat_14, 0);
    lv_obj_center(btn_label);

    bsp_display_unlock();
    ESP_LOGI(TAG, "ui ready");
    return ESP_OK;
}

void xz_ui_set_state(const char *state)
{
    if (s_state == NULL || bsp_display_lock(UI_LOCK_MS) != ESP_OK) {
        return;
    }
    set_text_if_changed(s_state, state);
    bsp_display_unlock();
}

void xz_ui_set_detail(const char *text)
{
    if (s_detail == NULL || bsp_display_lock(UI_LOCK_MS) != ESP_OK) {
        return;
    }
    set_text_if_changed(s_detail, text);
    bsp_display_unlock();
}

void xz_ui_set_code(const char *code)
{
    if (s_code == NULL || bsp_display_lock(UI_LOCK_MS) != ESP_OK) {
        return;
    }
    set_text_if_changed(s_code, code);
    bsp_display_unlock();
}
