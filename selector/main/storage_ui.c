#include "storage_ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"

#include "cjk_font.h"
#include "image_store.h"
#include "tap_gesture.h"
#include "ui_scroll_fx.h"
#include "ui_theme.h"

static const char *TAG = "selector.storage";

/* 与 pack_storage.ps1 的 id 上限对齐，够用即可 */
#define MAX_ROWS        16

/* 与设置页同一套滚动效果：中间一行放大、上下边缘渐隐 */
static const ui_scroll_fx_cfg_t STORAGE_FX = {
    .fade_top = 48,
    .fade_bottom = 40,
    .focus_scale = 288,
    .idle_scale = 228,
    .focus_opa = LV_OPA_COVER,
    .idle_opa = 225,
};

static lv_obj_t *s_root;
static lv_obj_t *s_sum_title;
static lv_obj_t *s_sum_bar;
static lv_obj_t *s_sum_sub;
static lv_obj_t *s_list;

/* LVGL 自带 snprintf 不支持 %f（CONFIG_LV_USE_FLOAT=n），小数位用整数算 */
static void format_mb(char *buf, size_t len, size_t bytes)
{
    const size_t unit = 1024u * 1024u;
    snprintf(buf, len, "%u.%u",
             (unsigned)(bytes / unit),
             (unsigned)(((bytes % unit) * 10u) / unit));
}

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

/* -------------------------------------------------------------------------- */

/** @brief 列表里的一行：左边名称+版本，右边占用空间 */
static void add_row(const char *title, const char *sub, uint32_t title_color, const char *right)
{
    lv_obj_t *row = lv_obj_create(s_list);
    lv_obj_set_size(row, 320, 46);
    /* 无底色无边框：和设置页一致，内容直接坐在背景渐变上 */
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_outline_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_scrollable(row, false);
    lv_obj_set_clickable(row, false);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, title);
    lv_obj_set_style_text_color(name, lv_color_hex(title_color), 0);
    lv_obj_set_style_text_font(name, cjk_font_or(16, &lv_font_montserrat_16), 0);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 14, -8);

    lv_obj_t *detail = lv_label_create(row);
    lv_label_set_text(detail, sub);
    lv_obj_set_style_text_color(detail, lv_color_hex(UI_COLOR_MUTED), 0);
    lv_obj_set_style_text_font(detail, cjk_font_or(14, &lv_font_montserrat_14), 0);
    lv_obj_align(detail, LV_ALIGN_LEFT_MID, 14, 10);

    lv_obj_t *size = lv_label_create(row);
    lv_label_set_text(size, right);
    lv_obj_set_style_text_color(size, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(size, &lv_font_montserrat_14, 0);
    lv_obj_align(size, LV_ALIGN_RIGHT_MID, -14, 0);
}

/* 每次打开都重新扫一遍：pack_storage 之后不重启也能看到最新列表 */
static void rebuild(void)
{
    lv_obj_clean(s_list);

    image_entry_t entries[MAX_ROWS];
    const int count = image_store_scan(entries, MAX_ROWS);

    size_t total = 0;
    size_t used = 0;
    image_store_info(&total, &used);

    const unsigned pct = (total == 0) ? 0u : (unsigned)(((uint64_t)used * 100u) / total);
    lv_bar_set_value(s_sum_bar, (int32_t)pct, LV_ANIM_OFF);

    char used_mb[12];
    char total_mb[12];
    char left_mb[12];
    format_mb(used_mb, sizeof(used_mb), used);
    format_mb(total_mb, sizeof(total_mb), total);
    format_mb(left_mb, sizeof(left_mb), (used < total) ? (total - used) : 0u);

    char buf[64];
    snprintf(buf, sizeof(buf), "已用 %s / %s MB  %u%%", used_mb, total_mb, pct);
    lv_label_set_text(s_sum_title, buf);

    snprintf(buf, sizeof(buf), "剩余 %s MB · %d 个应用", left_mb, count);
    lv_label_set_text(s_sum_sub, buf);

    if (count == 0) {
        add_row("镜像库为空", "用 tools/pack_storage.ps1 打包后刷 storage", UI_COLOR_MUTED, "-");
        ESP_LOGI(TAG, "empty library, %u/%u bytes used", (unsigned)used, (unsigned)total);
        ui_scroll_fx_refresh(s_list);
        return;
    }

    size_t apps_bytes = 0;
    for (int i = 0; i < count; i++) {
        apps_bytes += entries[i].size;

        char sub[80];
        /* 显式限长：字段来自 app 描述符，理论上不会超，但截断分析会报警告 */
        snprintf(sub, sizeof(sub), "v%.32s  %.39s", entries[i].version, entries[i].file);

        char size_mb[12];
        format_mb(size_mb, sizeof(size_mb), entries[i].size);

        char right[16];
        /* 小于 1 MB 的用 KB 显示，免得全是 0.x */
        if (entries[i].size < 1024u * 1024u) {
            snprintf(right, sizeof(right), "%u KB", (unsigned)(entries[i].size / 1024u));
        } else {
            snprintf(right, sizeof(right), "%s MB", size_mb);
        }

        add_row(entries[i].name, sub, UI_COLOR_TEXT, right);
    }

    /* 字库等非 app 文件：总量对不上时给一行解释，免得用户以为算错了 */
    if (used > apps_bytes) {
        char size_mb[12];
        format_mb(size_mb, sizeof(size_mb), used - apps_bytes);

        char right[16];
        snprintf(right, sizeof(right), "%s MB", size_mb);
        add_row("共享资源", "字库等非 app 文件", UI_COLOR_MUTED, right);
    }

    ESP_LOGI(TAG, "%d app(s), app %u B, used %u B / %u B",
             count, (unsigned)apps_bytes, (unsigned)used, (unsigned)total);

    /* 整批重建后位置变了，重新算一遍淡出 */
    ui_scroll_fx_refresh(s_list);
}

/* -------------------------------------------------------------------------- */

static void create_root(void)
{
    lv_obj_t *screen = lv_screen_active();

    s_root = lv_obj_create(screen);
    lv_obj_set_size(s_root, 466, 466);
    lv_obj_center(s_root);
    ui_bg_apply(s_root);
    lv_obj_set_scrollable(s_root, false);
    lv_obj_set_clickable(s_root, true);   /* 挡住下层点击 */
    lv_obj_set_hidden(s_root, true);

    lv_obj_t *title = lv_label_create(s_root);
    lv_label_set_text(title, "存储");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(title, cjk_font_or(20, &lv_font_montserrat_20), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    /* 汇总块：一行用量文字 + 进度条 + 剩余/应用数 */
    lv_obj_t *summary = lv_obj_create(s_root);
    lv_obj_set_size(summary, 320, 76);
    lv_obj_align(summary, LV_ALIGN_TOP_MID, 0, 86);
    lv_obj_set_style_bg_color(summary, lv_color_hex(UI_COLOR_ROW), 0);
    lv_obj_set_style_bg_opa(summary, LV_OPA_80, 0);
    lv_obj_set_style_radius(summary, 16, 0);
    lv_obj_set_style_border_width(summary, 1, 0);
    lv_obj_set_style_border_color(summary, lv_color_hex(UI_COLOR_ROW_EDGE), 0);
    lv_obj_set_style_pad_all(summary, 0, 0);
    lv_obj_set_scrollable(summary, false);
    lv_obj_set_clickable(summary, false);

    s_sum_title = lv_label_create(summary);
    lv_label_set_text(s_sum_title, "");
    lv_obj_set_style_text_color(s_sum_title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(s_sum_title, cjk_font_or(16, &lv_font_montserrat_16), 0);
    lv_obj_align(s_sum_title, LV_ALIGN_TOP_LEFT, 16, 12);

    s_sum_bar = lv_bar_create(summary);
    lv_obj_set_size(s_sum_bar, 288, 8);
    lv_obj_align(s_sum_bar, LV_ALIGN_TOP_MID, 0, 38);
    lv_bar_set_range(s_sum_bar, 0, 100);
    lv_obj_set_style_radius(s_sum_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_sum_bar, lv_color_hex(UI_COLOR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_sum_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_sum_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_sum_bar, lv_color_hex(UI_COLOR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(s_sum_bar, lv_color_hex(UI_COLOR_ACCENT_2), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(s_sum_bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);

    s_sum_sub = lv_label_create(summary);
    lv_label_set_text(s_sum_sub, "");
    lv_obj_set_style_text_color(s_sum_sub, lv_color_hex(UI_COLOR_MUTED), 0);
    lv_obj_set_style_text_font(s_sum_sub, cjk_font_or(14, &lv_font_montserrat_14), 0);
    lv_obj_align(s_sum_sub, LV_ALIGN_TOP_LEFT, 16, 50);

    /* app 列表：竖着滚，行高 46 + 间距 8 */
    s_list = lv_obj_create(s_root);
    lv_obj_set_size(s_list, 420, 188);   /* 行会放大到 125%，容器要留出余量 */
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 172);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    ui_scroll_fx_attach(s_list, &STORAGE_FX);

    /* 返回：标题左边只留图标，和设置页保持一致 */
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

void storage_ui_open(void)
{
    if (s_root == NULL) {
        create_root();
    }
    rebuild();
    lv_obj_set_hidden(s_root, false);
    lv_obj_move_to_index(s_root, -1);
}
