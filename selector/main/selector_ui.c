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
#include "screen_power.h"
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

/* ---------------------------------- 网格 ---------------------------------- */

/*
 * 圆屏（466x466，内切圆半径 233）下 2x2 磁贴的尺寸：
 *   150 x 150，间距 18 → 整块 318x318，四角到圆心 √(159²+159²)=225 < 233，
 *   磁贴还能被手指点到；再大就会顶到圆弧上。
 */
#define TILE_SIZE       132
#define TILE_GAP        20
#define TILE_STEP       (TILE_SIZE + TILE_GAP)
#define TILE_OFF        (TILE_STEP / 2)         /* 每格中心相对屏幕中心的偏移：84 */
#define TILES_PER_PAGE  4
#define PAGER_MAX_PAGES 8
#define PAGE_DOT_Y      187
#define PAGE_DOT_GAP    14

/** @brief 已知 app 的图标外观（id 来自 app-<id>-<版本>.bin） */
typedef struct {
    const char *id;
    const char *title;      /* 磁贴上的名字 */
    const char *glyph;      /* 烙印图案（UTF-8，汉字或 LV_SYMBOL_*） */
    uint32_t    color;      /* 纯色底 */
} app_look_t;

static const app_look_t APP_LOOKS[] = {
    { "watch",   "怀表", "表", 0x2F6BFF },
    { "gmeter",  "骑行", "骑", 0x18A56B },
    /* title 为空串 = 图标本身就能说明白（笑脸头像），磁贴不再画文字 */
    { "xiaozhi", "",     "智", 0x7C5CFF },
};

/* 未知 app 按文件名哈希取一个底色，保证同一份镜像永远是同一个颜色 */
static const uint32_t TILE_PALETTE[] = {
    0x2F6BFF, 0x18A56B, 0xD9662B, 0xA051D8, 0xC79A17, 0x2FA8C7, 0xC2455B, 0x4C8C2B,
};

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

/* 应用网格：横向分页，每页 2x2 个磁贴（最后一格是设置） */
static lv_obj_t *s_pager = NULL;
static lv_obj_t *s_dots[PAGER_MAX_PAGES];
static int s_page_count = 0;
static int s_dot_count = 0;     /* 实际创建的页码点；只有 1 页时不建 */

/* 所有磁贴：翻页时统一做缩放/淡出 */
static lv_obj_t *s_tiles[TILES_PER_PAGE * PAGER_MAX_PAGES];
static lv_image_dsc_t s_icon_dsc[TILES_PER_PAGE * PAGER_MAX_PAGES];
static int s_tile_count = 0;

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

static void on_tile_released(lv_event_t *event)
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
/*  应用磁贴                                                                   */
/* -------------------------------------------------------------------------- */

/** @brief 已知 app 的外观（按 app-<id>-<版本>.bin 里的 id 匹配），未知返回 NULL */
static const app_look_t *lookup_app(const char *file)
{
    if (strncmp(file, "app-", 4) != 0) {
        return NULL;
    }

    for (size_t i = 0; i < sizeof(APP_LOOKS) / sizeof(APP_LOOKS[0]); i++) {
        const size_t id_len = strlen(APP_LOOKS[i].id);
        if (strncmp(file + 4, APP_LOOKS[i].id, id_len) == 0 && file[4 + id_len] == '-') {
            return &APP_LOOKS[i];
        }
    }
    return NULL;
}

/** @brief 未知 app 的底色：文件名哈希到固定调色板（同一份镜像永远同色） */
static uint32_t palette_color(const char *key)
{
    uint32_t hash = 5381u;
    for (const char *p = key; *p != '\0'; p++) {
        hash = hash * 33u + (uint8_t)*p;
    }
    return TILE_PALETTE[hash % (sizeof(TILE_PALETTE) / sizeof(TILE_PALETTE[0]))];
}

/** @brief pct=100 原色；<100 变暗（刻痕）；>100 变亮（高光） */
static uint32_t shade_color(uint32_t color, unsigned pct)
{
    unsigned r = ((color >> 16) & 0xFFu) * pct / 100u;
    unsigned g = ((color >> 8) & 0xFFu) * pct / 100u;
    unsigned b = (color & 0xFFu) * pct / 100u;
    if (r > 255u) {
        r = 255u;
    }
    if (g > 255u) {
        g = 255u;
    }
    if (b > 255u) {
        b = 255u;
    }
    return (r << 16) | (g << 8) | b;
}

/**
 * @brief 在磁贴上"烙印"图案
 *
 * 两层叠出来：先画一层比底色亮、向下偏 2px 的字当高光，再压一层比底色暗的字，
 * 看起来像刻进去的凹凸印，而不是浮在上面的一张图。
 */
static void tile_stamp(lv_obj_t *tile, const char *glyph, uint32_t color, const lv_font_t *font)
{
    lv_obj_t *hi = lv_label_create(tile);
    lv_label_set_text(hi, glyph);
    lv_obj_set_style_text_font(hi, font, 0);
    lv_obj_set_style_text_color(hi, lv_color_hex(shade_color(color, 155)), 0);
    lv_obj_align(hi, LV_ALIGN_CENTER, 0, -12);

    lv_obj_t *lo = lv_label_create(tile);
    lv_label_set_text(lo, glyph);
    lv_obj_set_style_text_font(lo, font, 0);
    lv_obj_set_style_text_color(lo, lv_color_hex(shade_color(color, 50)), 0);
    lv_obj_align(lo, LV_ALIGN_CENTER, 0, -10);
}

static lv_obj_t *create_tile(lv_obj_t *page, int32_t x, int32_t y, uint32_t color)
{
    lv_obj_t *tile = lv_obj_create(page);
    lv_obj_set_size(tile, TILE_SIZE, TILE_SIZE);
    lv_obj_set_style_radius(tile, 32, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_set_style_shadow_width(tile, 16, 0);
    lv_obj_set_style_shadow_opa(tile, LV_OPA_30, 0);
    lv_obj_set_style_shadow_color(tile, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_ofs_y(tile, 6, 0);
    lv_obj_set_scrollable(tile, false);
    lv_obj_set_clickable(tile, true);
    lv_obj_align(tile, LV_ALIGN_CENTER, x, y);
    tap_gesture_attach(tile);

    if (s_tile_count < (int)(sizeof(s_tiles) / sizeof(s_tiles[0]))) {
        s_tiles[s_tile_count++] = tile;
    }
    return tile;
}

/** @brief 磁贴底部一行名字（版本号在存储页看） */
static void tile_caption(lv_obj_t *tile, const char *title)
{
    lv_obj_t *name = lv_label_create(tile);
    lv_obj_set_width(name, TILE_SIZE - 16);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_label_set_text(name, title);
    lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(name, cjk_font_or(16, &lv_font_montserrat_16), 0);
    lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static void create_app_tile(lv_obj_t *page, const image_entry_t *entry, int index,
                            int32_t x, int32_t y)
{
    const app_look_t *look = lookup_app(entry->file);
    const uint32_t color = (look != NULL) ? look->color : palette_color(entry->file);

    /* 空标题 = 不画底部文字，图标在磁贴里居中占满 */
    const char *caption = (look != NULL) ? look->title : entry->name;
    const bool has_caption = (caption != NULL && caption[0] != '\0');

    lv_obj_t *tile = create_tile(page, x, y, color);
    lv_obj_add_event_cb(tile, on_tile_released, LV_EVENT_RELEASED, (void *)(intptr_t)index);

    /*
     * 优先用 app 自带的图标（pack_storage 把 icon-<id>.png 转成 LVGL 二进制），
     * 没有图标才回退到"烙印汉字/首字母"。
     */
    uint8_t *icon_data = NULL;
    size_t icon_size = 0;

    if (image_store_read_icon(entry->file, &icon_data, &icon_size)) {
        const size_t slot = (size_t)(s_tile_count - 1);
        lv_image_dsc_t *dsc = &s_icon_dsc[slot];

        dsc->header = *(const lv_image_header_t *)icon_data;
        dsc->data_size = (uint32_t)(icon_size - sizeof(lv_image_header_t));
        dsc->data = icon_data + sizeof(lv_image_header_t);

        lv_obj_t *img = lv_image_create(tile);
        lv_image_set_src(img, dsc);
        lv_obj_align(img, LV_ALIGN_CENTER, 0, has_caption ? -12 : 0);
        ESP_LOGI(TAG, "%s: icon %dx%d", entry->file, (int)dsc->header.w, (int)dsc->header.h);
    } else {
        char fallback[4] = { 0 };
        const char *glyph = NULL;
        const lv_font_t *font = NULL;

        if (look != NULL) {
            glyph = look->glyph;
            font = cjk_font_or(44, &lv_font_montserrat_40);
        } else {
            /* 未知 app：用项目名首字母，ASCII 直接用 Montserrat，避免中文字库缺字形 */
            char c = (entry->name[0] != '\0') ? entry->name[0] : '?';
            if (c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
            }
            fallback[0] = c;
            glyph = fallback;
            font = &lv_font_montserrat_40;
        }
        tile_stamp(tile, glyph, color, font);
    }

    if (has_caption) {
        tile_caption(tile, caption);
    }
}

/* 设置也是最后一格磁贴，和 app 平级 */
static void create_settings_tile(lv_obj_t *page, int32_t x, int32_t y)
{
    const uint32_t color = 0x6E6E92;

    lv_obj_t *tile = create_tile(page, x, y, color);
    lv_obj_add_event_cb(tile, on_settings_released, LV_EVENT_RELEASED, NULL);

    /* 齿轮符号只有 Montserrat 有字形，不能用中文字体 */
    tile_stamp(tile, LV_SYMBOL_SETTINGS, color, &lv_font_montserrat_40);
    tile_caption(tile, "设置");
}

/**
 * @brief 按磁贴离屏幕中心的距离做缩放/淡出
 *
 * 只在翻页过程中调用（SCROLL 事件），停下时 offset=0 → 全部 100%，
 * 所以不会留下长期缩放导致的模糊。整页偏出时最多缩到 95%、淡到 78%。
 */
static void tiles_apply_focus(void)
{
    const int32_t center = 466 / 2;

    for (int i = 0; i < s_tile_count; i++) {
        lv_area_t area;
        lv_obj_get_coords(s_tiles[i], &area);

        int32_t dist = (area.x1 + area.x2) / 2 - center;
        if (dist < 0) {
            dist = -dist;
        }
        if (dist > 466) {
            dist = 466;
        }

        const int32_t scale = 256 - (dist * 13) / 466;          /* 256 → 243 */
        const lv_opa_t opa = (lv_opa_t)(255 - (dist * 56) / 466);   /* 255 → 199 */

        lv_obj_set_style_transform_scale_x(s_tiles[i], scale, 0);
        lv_obj_set_style_transform_scale_y(s_tiles[i], scale, 0);
        lv_obj_set_style_opa(s_tiles[i], opa, 0);
    }
}

static void on_pager_scrolled(lv_event_t *event)
{
    (void)event;
    tiles_apply_focus();
}

static void dots_update(int page)
{
    for (int i = 0; i < s_dot_count; i++) {
        const uint32_t color = (i == page) ? UI_COLOR_ACCENT_2 : UI_COLOR_DIM;
        lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(color), 0);
    }
}

static void on_pager_scroll_end(lv_event_t *event)
{
    (void)event;

    const int32_t x = lv_obj_get_scroll_x(s_pager);
    int page = (x + 466 / 2) / 466;
    if (page < 0) {
        page = 0;
    } else if (page >= s_page_count) {
        page = s_page_count - 1;
    }

    /* 吸附可能停在整页 ±1~2 px，统一对齐，避免内容看着"偏了一点" */
    const int32_t target = page * 466;
    if (x != target) {
        lv_obj_scroll_to_x(s_pager, target, LV_ANIM_OFF);
    }
    dots_update(page);
    tiles_apply_focus();    /* 停下后回到 100% 大小 / 全不透明 */
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

    /* 电源键单击 = 息屏/亮屏；息屏期间触摸设备被禁用，界面不响应任何点击 */
    if (battery_power_key_clicked()) {
        screen_power_toggle();
    }

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
    s_tile_count = 0;

    lv_obj_t *screen = lv_screen_active();
    ui_bg_apply(screen);
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

    /*
     * 应用网格：横向分页，每页 2x2 个磁贴，最后一格固定是设置。
     * 用"可横向滚动的容器 + 吸附"来实现翻页，省掉自己写手势识别。
     */
    const int tile_total = count + 1;
    s_page_count = (tile_total + TILES_PER_PAGE - 1) / TILES_PER_PAGE;
    if (s_page_count > PAGER_MAX_PAGES) {
        s_page_count = PAGER_MAX_PAGES;
    }

    s_pager = lv_obj_create(screen);
    lv_obj_set_size(s_pager, 466, 466);
    lv_obj_center(s_pager);
    lv_obj_set_style_bg_opa(s_pager, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pager, 0, 0);
    lv_obj_set_style_radius(s_pager, 0, 0);
    lv_obj_set_style_pad_all(s_pager, 0, 0);
    lv_obj_set_scroll_dir(s_pager, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(s_pager, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(s_pager, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(s_pager, LV_FLEX_FLOW_ROW);
    lv_obj_add_event_cb(s_pager, on_pager_scrolled, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(s_pager, on_pager_scroll_end, LV_EVENT_SCROLL_END, NULL);

    for (int page = 0; page < s_page_count; page++) {
        lv_obj_t *page_obj = lv_obj_create(s_pager);
        lv_obj_set_size(page_obj, 466, 466);
        lv_obj_set_style_bg_opa(page_obj, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(page_obj, 0, 0);
        lv_obj_set_style_radius(page_obj, 0, 0);
        lv_obj_set_style_pad_all(page_obj, 0, 0);
        lv_obj_set_scrollable(page_obj, false);

        if (count == 0) {
            /* 空库：提示 + 一个居中的设置磁贴，否则右上角孤零零一格很奇怪 */
            lv_obj_t *hint = lv_label_create(page_obj);
            lv_label_set_text(hint, "镜像库为空\n\n用 tools/pack_storage.ps1 打包\n再刷 storage 分区");
            lv_obj_set_style_text_color(hint, lv_color_hex(COLOR_MUTED), 0);
            lv_obj_set_style_text_font(hint, cjk_font_or(14, &lv_font_montserrat_14), 0);
            lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(hint, LV_ALIGN_CENTER, 0, -110);
            create_settings_tile(page_obj, 0, 50);
            continue;
        }

        for (int slot = 0; slot < TILES_PER_PAGE; slot++) {
            const int idx = page * TILES_PER_PAGE + slot;
            if (idx > count) {
                break;                  /* idx == count 是设置格，再往后就没有了 */
            }

            const int32_t x = (slot % 2 == 0) ? -TILE_OFF : TILE_OFF;
            const int32_t y = (slot < 2) ? -TILE_OFF : TILE_OFF;

            if (idx < count) {
                create_app_tile(page_obj, &images[idx], idx, x, y);
            } else {
                create_settings_tile(page_obj, x, y);
            }
        }
    }

    /* 页码点：画在 pager 之后，保证在最上层且不跟着滑动；只有一页就不放点 */
    s_dot_count = (s_page_count > 1) ? s_page_count : 0;
    for (int i = 0; i < s_dot_count; i++) {
        s_dots[i] = lv_obj_create(screen);
        lv_obj_set_size(s_dots[i], 6, 6);
        lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_dots[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_dots[i], lv_color_hex(UI_COLOR_DIM), 0);
        lv_obj_set_style_border_width(s_dots[i], 0, 0);
        lv_obj_set_style_pad_all(s_dots[i], 0, 0);
        lv_obj_set_scrollable(s_dots[i], false);
        lv_obj_align(s_dots[i], LV_ALIGN_CENTER,
                     (int32_t)(i - (s_page_count - 1) / 2) * PAGE_DOT_GAP, PAGE_DOT_Y);
    }
    dots_update(0);
    tiles_apply_focus();    /* 初始状态：全部 100% */

    /* 存储用量：选中的行写在设置页，这里只留日志方便排查 */
    ESP_LOGI(TAG, "storage %u/%u bytes, %d app(s), %d page(s)",
             (unsigned)fs_used, (unsigned)fs_total, count, s_page_count);

    create_overlay(screen);

    s_created_tick = lv_tick_get();
    lv_timer_create(poll_timer_cb, 100, NULL);
    lv_timer_create(status_timer_cb, 1000, NULL);
    status_timer_cb(NULL);   /* 首帧就带上时间/电量，不用等 1 s */
}
