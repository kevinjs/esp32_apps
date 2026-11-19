#include "xz_ui.h"

#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "cjk_font.h"
#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "xz.ui";

#define COL_BG      0x000000
#define COL_TEXT    0xFFFFFF
#define COL_MUTED   0x808080
#define COL_ACCENT  0x2ECC71
#define COL_LISTEN  0x3498DB
#define COL_WARN    0xE67E22
#define COL_IDLE_BTN 0x3A3A3A

#define UI_LOCK_MS  1000

/* 触摸控制器上电时会给出一次假触摸，界面刚出现的这段时间忽略点击 */
#define CLICK_GUARD_MS  1000

/*
 * 会话文本（识别结果 / 播报文本）是中文，必须用 CJK 字库，
 * Montserrat 只有拉丁字形，中文会渲染成方块。
 */
#if LV_FONT_SOURCE_HAN_SANS_SC_16_CJK
#define XZ_FONT_TEXT    (&lv_font_source_han_sans_sc_16_cjk)
#else
#define XZ_FONT_TEXT    (&lv_font_montserrat_16)
#endif

/*
 * 中文渲染：用共享组件 components/cjk_font，字库文件放在 storage 分区
 * （font-cjk.ttf，GB2312 子集），selector 与所有 app 共用一份，不再各自
 * 把 2MB 字库编进镜像。
 * 兜底顺序：storage 字库 -> 内置 1195 字位图字库 -> Montserrat。
 */
#define XZ_FONT_TEXT_PX     18
#define XZ_FONT_STATE_PX    26

/* 对话区保留的最近条数（圆形屏幕上大约 3~4 屏，够上下翻） */
#define CHAT_MAX_ROWS       12

static const lv_font_t *s_font_text = NULL;
static const lv_font_t *s_font_state = NULL;

static void fonts_init(void)
{
    if (cjk_font_init() != ESP_OK) {
        ESP_LOGW(TAG, "shared CJK font unavailable (storage/font-cjk.ttf), falling back");
    }

    s_font_text = cjk_font_or(XZ_FONT_TEXT_PX, XZ_FONT_TEXT);
    s_font_state = cjk_font_or(XZ_FONT_STATE_PX, &lv_font_montserrat_28);
}

static lv_obj_t *s_state = NULL;
static lv_obj_t *s_detail = NULL;      /* 单行提示（连网中 / 没听清 等） */
static lv_obj_t *s_chat = NULL;        /* 可上下滑动的对话区 */
static lv_obj_t *s_msgs[CHAT_MAX_ROWS];/* 保留最近 N 条，超了删最旧 */
static int s_msg_count = 0;
static lv_obj_t *s_code = NULL;
static lv_obj_t *s_button = NULL;
static lv_obj_t *s_button_label = NULL;
static xz_ui_click_cb_t s_click_cb = NULL;
static uint32_t s_created_tick = 0;
static bool s_button_enabled = false;

static void set_text_if_changed(lv_obj_t *label, const char *text);

/*
 * 按钮文案跟着会话状态走：用户一眼就知道现在点它是"开始说"/"说完了"/"打断"，
 * 还是在忙（灰色不可点）。
 */
typedef struct {
    const char *state;
    const char *label;
    uint32_t    color;
    bool        enabled;
} ui_button_map_t;

static const ui_button_map_t s_button_map[] = {
    { "BOOTING",      "启动中", COL_IDLE_BTN, false },
    { "WIFI",         "联网中", COL_IDLE_BTN, false },
    { "AUDIO",        "初始化", COL_IDLE_BTN, false },
    { "LINK",         "连接中", COL_IDLE_BTN, false },
    { "CONNECTING",   "连接中", COL_IDLE_BTN, false },
    { "CONNECTED",    "准备中", COL_IDLE_BTN, false },
    { "IDLE",         "说话",   COL_LISTEN,   true  },
    { "LISTENING",    "说完",   COL_ACCENT,   true  },
    { "THINKING",     "取消",   0x7F8C8D,     true  },
    { "SPEAKING",     "打断",   COL_WARN,     true  },
    { "DISCONNECTED", "重试",   COL_LISTEN,   true  },
    { "ERROR",        "重试",   COL_LISTEN,   true  },
    { "NO WIFI",      "重试",   COL_LISTEN,   true  },
    { "AUDIO ERROR",  "重试",   COL_LISTEN,   true  },
    { "SERVER ERROR", "重试",   COL_LISTEN,   true  },
};

/* -------------------------------------------------------------------------- */

static void button_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || s_click_cb == NULL) {
        return;
    }
    if (!s_button_enabled) {
        return;     /* 忙的时候按钮是灰的，点了也没用 */
    }
    if (lv_tick_get() - s_created_tick < CLICK_GUARD_MS) {
        return;
    }
    s_click_cb();
}

static void update_button(const char *state)
{
    if (s_button == NULL || s_button_label == NULL) {
        return;
    }

    const ui_button_map_t *hit = NULL;
    for (size_t i = 0; i < sizeof(s_button_map) / sizeof(s_button_map[0]); i++) {
        if (state != NULL && strcmp(s_button_map[i].state, state) == 0) {
            hit = &s_button_map[i];
            break;
        }
    }

    const char *label = hit ? hit->label : "…";
    const uint32_t color = hit ? hit->color : COL_IDLE_BTN;

    s_button_enabled = hit ? hit->enabled : false;
    set_text_if_changed(s_button_label, label);
    lv_obj_set_style_bg_color(s_button, lv_color_hex(color), 0);
    lv_obj_set_style_text_color(s_button_label, lv_color_hex(COL_TEXT), 0);
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

    fonts_init();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_scroll_dir(scr, LV_DIR_NONE);
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_state = lv_label_create(scr);
    lv_label_set_text(s_state, "BOOTING");
    lv_obj_set_style_text_color(s_state, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(s_state, s_font_state, 0);
    /* 顶部状态文案上移，给下面的对话区让出空间（圆屏 y=54 处可用宽度约 260 px） */
    lv_obj_align(s_state, LV_ALIGN_TOP_MID, 0, 54);

    s_code = lv_label_create(scr);
    lv_label_set_text(s_code, "");
    lv_obj_set_style_text_color(s_code, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_font(s_code, &lv_font_montserrat_28, 0);
    lv_obj_align(s_code, LV_ALIGN_CENTER, 0, -40);

    /*
     * 对话区：竖向 flex 的滚动容器，触摸上下滑即可回看。
     * 圆形屏幕，宽度留 360 保证 y=100 处不被圆边裁掉。
     */
    s_chat = lv_obj_create(scr);
    lv_obj_remove_style_all(s_chat);
    lv_obj_set_size(s_chat, 360, 196);
    lv_obj_align(s_chat, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_set_flex_flow(s_chat, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_chat, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_chat, 8, 0);
    lv_obj_set_style_pad_hor(s_chat, 4, 0);
    lv_obj_set_scroll_dir(s_chat, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_chat, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_width(s_chat, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_chat, LV_OPA_TRANSP, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(s_chat, lv_color_hex(COL_MUTED), LV_PART_SCROLLBAR);

    /* 单行提示：连接进度、错误原因、超时提醒 */
    s_detail = lv_label_create(scr);
    lv_label_set_text(s_detail, "");
    lv_obj_set_style_text_color(s_detail, lv_color_hex(COL_MUTED), 0);
    lv_obj_set_style_text_font(s_detail, s_font_text, 0);
    lv_obj_set_style_text_align(s_detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_detail, 360);
    lv_obj_align(s_detail, LV_ALIGN_TOP_MID, 0, 302);

    s_button = lv_obj_create(scr);
    lv_obj_set_size(s_button, 88, 88);
    lv_obj_set_style_radius(s_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_button, lv_color_hex(COL_LISTEN), 0);
    lv_obj_set_style_bg_opa(s_button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_button, 0, 0);
    lv_obj_align(s_button, LV_ALIGN_BOTTOM_MID, 0, -46);
    lv_obj_add_event_cb(s_button, button_event_cb, LV_EVENT_CLICKED, NULL);

    s_button_label = lv_label_create(s_button);
    lv_label_set_text(s_button_label, "");
    lv_obj_set_style_text_color(s_button_label, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(s_button_label, s_font_text, 0);
    lv_obj_center(s_button_label);
    update_button("BOOTING");

    s_created_tick = lv_tick_get();
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
    update_button(state);

    /*
     * 每轮对话结束（IDLE）做一次全屏重绘。
     * adapter 在 esp_lcd_panel_draw_bitmap 失败时照样 flush_ready，
     * 那块区域会被 LVGL 当成"已更新"而永久残留，只有整屏重绘能修掉。
     */
    if (state != NULL && strcmp(state, "IDLE") == 0) {
        lv_obj_invalidate(lv_screen_active());
    }
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

void xz_ui_add_message(bool from_user, const char *text)
{
    if (s_chat == NULL || text == NULL || bsp_display_lock(UI_LOCK_MS) != ESP_OK) {
        return;
    }

    /* 超出上限先删最旧的一条（删 row 会带走 tag/body） */
    if (s_msg_count >= CHAT_MAX_ROWS) {
        lv_obj_delete(s_msgs[0]);
        memmove(&s_msgs[0], &s_msgs[1], sizeof(s_msgs[0]) * (CHAT_MAX_ROWS - 1));
        s_msg_count--;
    }

    lv_obj_t *row = lv_obj_create(s_chat);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tag = lv_label_create(row);
    lv_label_set_text(tag, from_user ? "我" : "小智");
    lv_obj_set_style_text_color(tag, lv_color_hex(from_user ? COL_LISTEN : COL_ACCENT), 0);
    lv_obj_set_style_text_font(tag, s_font_text, 0);

    lv_obj_t *body = lv_label_create(row);
    lv_label_set_text(body, text);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(body, 1);      /* 占满剩余宽度，按需换行 */
    lv_obj_set_style_text_color(body, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(body, s_font_text, 0);

    s_msgs[s_msg_count++] = row;

    /* 有新内容就把"没听清"之类的提示清掉，并滚到最新一条 */
    set_text_if_changed(s_detail, "");
    lv_obj_update_layout(s_chat);
    lv_obj_scroll_to_view(row, LV_ANIM_OFF);

    bsp_display_unlock();
}

void xz_ui_clear_history(void)
{
    if (s_chat == NULL || bsp_display_lock(UI_LOCK_MS) != ESP_OK) {
        return;
    }
    for (int i = 0; i < s_msg_count; i++) {
        lv_obj_delete(s_msgs[i]);
        s_msgs[i] = NULL;
    }
    s_msg_count = 0;
    bsp_display_unlock();
}
