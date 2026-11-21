#include "xz_ui.h"

#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "cjk_font.h"
#include "esp_log.h"
#include "esp_random.h"
#include "face_avatar.h"
#include "lvgl.h"
#include "ui_bg.h"

static const char *TAG = "xz.ui";

/* 背景不再用纯黑：统一由 components/ui_bg 铺 selector 同款渐变 */
#define COL_TEXT    0xFFFFFF
#define COL_MUTED   0x808080
#define COL_ACCENT  0x2ECC71
#define COL_LISTEN  0x3498DB
#define COL_WARN    0xE67E22
#define COL_IDLE_BTN 0x3A3A3A
#define COL_FACE    0x1B1B29

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

/*
 * 表情：优先用 storage 里的共享表情包（components/face_avatar，素材 192 px，
 * selector 与所有 app 共用一份）；素材没打进镜像时回退到下面这组几何参数
 * 画的"简笔脸"，保证界面不会开天窗。
 */
#define FACE_PX         192     /* 共享表情包边长，打包时固定 192 */
#define FACE_SIZE       176     /* 几何回退脸的边长 */
#define EYE_W           18
#define EYE_H           30
#define EYE_TOP         56
#define EYE_DX          30
#define MOUTH_W         66
#define MOUTH_H         44
#define MOUTH_TOP       94

#define EYE_OPEN        256     /* 睁眼 */
#define EYE_HALF        110     /* 半闭 */
#define EYE_BLINK       40      /* 眨眼最扁的时候 */

static const lv_font_t *s_font_text = NULL;
static const lv_font_t *s_font_state = NULL;

static void fonts_init(void)
{
    if (cjk_font_init() != ESP_OK) {
        ESP_LOGW(TAG, "shared CJK font unavailable (storage/font-cjk.ttf), falling back");
    }

    /*
     * 这个字号只渲染固定文案（按钮、状态、提示），字形数量有界，
     * 用 storage 里的 TTF 可以覆盖 GB2312，生僻字不会变成"口"。
     *
     * 注意：不要再把动态长文本（聊天记录之类）交给它渲染。
     * tiny_ttf 会为每个新汉字缓存一份字形位图，缓存上限是
     * CONFIG_LV_TINY_TTF_CACHE_GLYPH_CNT（512 个字形），够把内部 RAM 吃干；
     * 内部 RAM 一没，esp-aes 连解密用的 200 字节 DMA 缓冲都申请不到，
     * 会话就会永久卡在 thinking→idle。多轮对话因此改成用表情表达。
     */
    s_font_text = cjk_font_or(XZ_FONT_TEXT_PX, XZ_FONT_TEXT);

    /* 顶部状态字只有十几个 ASCII 字符，用 TTF 不会把缓存撑起来 */
    s_font_state = cjk_font_or(XZ_FONT_STATE_PX, &lv_font_montserrat_28);
}

static lv_obj_t *s_state = NULL;
static lv_obj_t *s_detail = NULL;      /* 单行提示（连网中 / 没听清 等） */
static lv_obj_t *s_face = NULL;        /* 中间的表情（几何回退脸） */
static lv_obj_t *s_eye_l = NULL;
static lv_obj_t *s_eye_r = NULL;
static lv_obj_t *s_mouth = NULL;       /* lv_arc：下半圈是笑，上半圈是沮丧 */
static lv_timer_t *s_blink_timer = NULL;
static int32_t s_eye_scale = EYE_OPEN; /* 当前状态的睁眼程度，眨眼后回到这里 */
static lv_obj_t *s_code = NULL;
static lv_obj_t *s_button = NULL;
static lv_obj_t *s_button_label = NULL;
static xz_ui_click_cb_t s_click_cb = NULL;
static uint32_t s_created_tick = 0;
static bool s_button_enabled = false;

/*
 * 共享表情包（图片）这条路径的状态。
 * components/face_avatar 从 storage 读 face-*.bin 到 PSRAM 后，这里只管
 * "什么时候换哪张图"：播报换帧、待命眨眼、呼吸缩放。
 */
static bool s_image_face = false;
static lv_obj_t *s_face_img = NULL;
static lv_timer_t *s_mood_timer = NULL;
static face_avatar_mood_t s_mood = FACE_AVATAR_WAIT;
static int s_talk_idx = 0;
static uint32_t s_blink_until = 0;
static uint32_t s_next_blink = 0;

static void set_text_if_changed(lv_obj_t *label, const char *text);
static void mood_timer_cb(lv_timer_t *timer);

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
/* 表情：一张会动的脸，替代原来的文字对话稿                                    */

typedef enum {
    FACE_WAIT = 0,  /* 启动 / 联网 / 连接中：半闭眼等 */
    FACE_IDLE,      /* 待命：微笑 + 呼吸 */
    FACE_LISTEN,    /* 聆听：睁大眼 + 小圆嘴 */
    FACE_THINK,     /* 思考：眼睛上瞟 + 小嘴 */
    FACE_TALK,      /* 播报：嘴巴一张一合 */
    FACE_SAD,       /* 出错 / 断开：嘴角向下 */
} face_mood_t;

static void anim_scale_cb(void *var, int32_t v)
{
    lv_obj_set_style_transform_scale_x((lv_obj_t *)var, v, 0);
    lv_obj_set_style_transform_scale_y((lv_obj_t *)var, v, 0);
}

static void anim_scale_y_cb(void *var, int32_t v)
{
    lv_obj_set_style_transform_scale_y((lv_obj_t *)var, v, 0);
}

static void anim_opa_cb(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

/** @brief 跑一段缩放动画；loop=true 时来回摆动循环 */
static void anim_run(lv_obj_t *obj, lv_anim_exec_xcb_t cb, int32_t from, int32_t to,
                     uint32_t dur, bool loop)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, dur);
    if (loop) {
        lv_anim_set_playback_duration(&a, dur);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    }
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

/** @brief 眨一下眼：压扁再弹回当前状态的睁眼程度（半闭状态就不眨） */
static void blink_once(lv_obj_t *eye)
{
    if (eye == NULL || s_eye_scale < EYE_OPEN) {
        return;
    }
    lv_anim_delete(eye, anim_scale_y_cb);
    anim_run(eye, anim_scale_y_cb, s_eye_scale, EYE_BLINK, 90, false);
}

static void blink_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    blink_once(s_eye_l);
    blink_once(s_eye_r);
}

static lv_obj_t *make_eye(lv_obj_t *parent, int32_t dx)
{
    lv_obj_t *eye = lv_obj_create(parent);
    lv_obj_set_size(eye, EYE_W, EYE_H);
    lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(eye, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(eye, 0, 0);
    lv_obj_set_style_pad_all(eye, 0, 0);
    lv_obj_set_scrollable(eye, false);
    lv_obj_align(eye, LV_ALIGN_TOP_MID, dx, EYE_TOP);
    lv_obj_set_style_transform_pivot_x(eye, EYE_W / 2, 0);
    lv_obj_set_style_transform_pivot_y(eye, EYE_H / 2, 0);
    return eye;
}

/* ---- 共享表情包（图片） --------------------------------------------------- */

static face_avatar_mood_t avatar_of_mood(face_mood_t mood)
{
    switch (mood) {
    case FACE_IDLE:
        return FACE_AVATAR_IDLE;
    case FACE_LISTEN:
        return FACE_AVATAR_LISTEN;
    case FACE_THINK:
        return FACE_AVATAR_THINK;
    case FACE_TALK:
        return FACE_AVATAR_TALK;
    case FACE_SAD:
        return FACE_AVATAR_SAD;
    case FACE_WAIT:
    default:
        return FACE_AVATAR_WAIT;
    }
}

/** @brief 播报时轮播 talk 帧；待命时随机眨眼；其余状态静止。 */
static void mood_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_face_img == NULL) {
        return;
    }

    const uint32_t now = lv_tick_get();

    if (s_mood == FACE_AVATAR_TALK) {
        const int frames = face_avatar_talk_frames();
        if (frames > 1) {
            s_talk_idx = (s_talk_idx + 1) % frames;
            const lv_image_dsc_t *dsc = face_avatar_talk_frame(s_talk_idx);
            if (dsc != NULL) {
                lv_image_set_src(s_face_img, dsc);
            }
        }
        return;
    }

    if (s_mood != FACE_AVATAR_IDLE) {
        return;
    }

    if (s_blink_until != 0) {
        if ((int32_t)(now - s_blink_until) < 0) {
            return;         /* 还在眨眼 */
        }
        s_blink_until = 0;
        const lv_image_dsc_t *idle = face_avatar_get(FACE_AVATAR_IDLE);
        if (idle != NULL) {
            lv_image_set_src(s_face_img, idle);
        }
        return;
    }

    if ((int32_t)(now - s_next_blink) >= 0) {
        const lv_image_dsc_t *blink = face_avatar_get(FACE_AVATAR_BLINK);
        if (blink != NULL) {
            lv_image_set_src(s_face_img, blink);
            s_blink_until = now + 130;
        }
        s_next_blink = now + 3000 + (esp_random() % 3000);
    }
}

static void face_image_set_mood(face_avatar_mood_t mood)
{
    s_mood = mood;
    s_talk_idx = 0;
    s_blink_until = 0;

    const lv_image_dsc_t *dsc = NULL;
    if (mood == FACE_AVATAR_TALK) {
        dsc = face_avatar_talk_frame(0);
    }
    if (dsc == NULL) {
        dsc = face_avatar_get(mood);
    }
    if (dsc == NULL) {
        dsc = face_avatar_get(FACE_AVATAR_IDLE);    /* 素材不全时兜底 */
    }
    if (dsc != NULL) {
        lv_image_set_src(s_face_img, dsc);
    }

    /*
     * 呼吸效果刻意不用 transform_scale：LVGL 的软件变换要实时重采样，
     * 会为 192×192 的图申请一块约 74 KB 的中间缓冲（落在内部 RAM），
     * 把刷屏要用的 DMA 连续块挤掉（实测 dma_max 137 KB → 31 KB，
     * 低于单次刷屏的 46 KB，久了会出现残影/停更）。
     * 改用透明度呼吸：混合在目标缓冲上逐像素做，不额外占内存。
     */
    lv_anim_delete(s_face_img, anim_scale_cb);
    lv_anim_delete(s_face_img, anim_opa_cb);
    lv_obj_set_style_transform_scale_x(s_face_img, 256, 0);
    lv_obj_set_style_transform_scale_y(s_face_img, 256, 0);
    lv_obj_set_style_opa(s_face_img, LV_OPA_COVER, 0);

    if (mood != FACE_AVATAR_TALK) {
        /* 播报时每 200 ms 换一帧，再叠透明度会闪，保持常亮 */
        anim_run(s_face_img, anim_opa_cb, LV_OPA_80, LV_OPA_COVER, 1800, true);
    }

    if (mood == FACE_AVATAR_IDLE) {
        s_next_blink = lv_tick_get() + 2500 + (esp_random() % 2500);
    }
}

static void face_image_create(lv_obj_t *parent)
{
    s_face_img = lv_image_create(parent);
    lv_obj_set_size(s_face_img, FACE_PX, FACE_PX);
    lv_obj_align(s_face_img, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_set_style_transform_pivot_x(s_face_img, FACE_PX / 2, 0);
    lv_obj_set_style_transform_pivot_y(s_face_img, FACE_PX / 2, 0);

    const lv_image_dsc_t *dsc = face_avatar_get(FACE_AVATAR_WAIT);
    if (dsc == NULL) {
        dsc = face_avatar_get(FACE_AVATAR_IDLE);
    }
    if (dsc != NULL) {
        lv_image_set_src(s_face_img, dsc);
    }

    /* 200 ms 一条定时器：播报换帧 + 眨眼计时都走这里，省一个 timer */
    s_mood_timer = lv_timer_create(mood_timer_cb, 200, NULL);
    ESP_LOGI(TAG, "shared avatar %ux%u, %d talk frames",
             (unsigned)face_avatar_size(), (unsigned)face_avatar_size(),
             face_avatar_talk_frames());
}

static void face_create(lv_obj_t *parent)
{
    /*
     * 优先用 storage 里的共享表情包（selector 与所有 app 共用的拟人形象）。
     * 素材缺失时（storage 没刷、或打包时漏了 faces/）回退到几何简笔脸。
     */
    if (face_avatar_init() == ESP_OK && face_avatar_ready()) {
        s_image_face = true;
        face_image_create(parent);
        return;
    }
    ESP_LOGW(TAG, "shared avatar unavailable, using built-in face");

    s_face = lv_obj_create(parent);
    lv_obj_set_size(s_face, FACE_SIZE, FACE_SIZE);
    lv_obj_align(s_face, LV_ALIGN_TOP_MID, 0, 104);
    lv_obj_set_style_radius(s_face, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_face, lv_color_hex(COL_FACE), 0);
    lv_obj_set_style_bg_opa(s_face, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_face, 2, 0);
    lv_obj_set_style_border_color(s_face, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_pad_all(s_face, 0, 0);
    lv_obj_set_scrollable(s_face, false);
    lv_obj_set_style_transform_pivot_x(s_face, FACE_SIZE / 2, 0);
    lv_obj_set_style_transform_pivot_y(s_face, FACE_SIZE / 2, 0);

    s_eye_l = make_eye(s_face, -EYE_DX);
    s_eye_r = make_eye(s_face, EYE_DX);

    /* 嘴用 lv_arc 画：只留背景弧，靠角度区分表情（下弧=笑，上弧=沮丧） */
    s_mouth = lv_arc_create(s_face);
    lv_obj_set_size(s_mouth, MOUTH_W, MOUTH_H);
    lv_obj_align(s_mouth, LV_ALIGN_TOP_MID, 0, MOUTH_TOP);
    lv_obj_remove_style(s_mouth, NULL, LV_PART_KNOB);
    lv_obj_remove_style(s_mouth, NULL, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_mouth, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_mouth, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_mouth, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_mouth, true, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_mouth, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_x(s_mouth, MOUTH_W / 2, 0);
    lv_obj_set_style_transform_pivot_y(s_mouth, MOUTH_H / 2, 0);
    lv_arc_set_bg_angles(s_mouth, 30, 150);
    lv_arc_set_value(s_mouth, 0);

    s_blink_timer = lv_timer_create(blink_timer_cb, 3600, NULL);
}

static void face_set_mood(face_mood_t mood)
{
    if (s_image_face) {
        face_image_set_mood(avatar_of_mood(mood));
        return;
    }
    if (s_face == NULL) {
        return;
    }

    /* 先停掉上一状态的动画，再按新状态重配，避免两个动画抢同一个属性 */
    lv_anim_delete(s_face, anim_scale_cb);
    lv_anim_delete(s_mouth, anim_scale_y_cb);
    lv_anim_delete(s_eye_l, anim_scale_y_cb);
    lv_anim_delete(s_eye_r, anim_scale_y_cb);

    int32_t eye = EYE_OPEN;
    int32_t eye_top = EYE_TOP;
    int32_t mouth_w = MOUTH_W;
    int32_t mouth_h = MOUTH_H;
    int32_t mouth_top = MOUTH_TOP;
    int32_t arc_w = 8;
    uint32_t mouth_color = COL_ACCENT;
    int32_t a1 = 30;
    int32_t a2 = 150;
    bool talk = false;

    switch (mood) {
    case FACE_WAIT:
        eye = EYE_HALF;
        mouth_color = COL_MUTED;
        a1 = 60;
        a2 = 120;                                   /* 一条平平的嘴 */
        break;
    case FACE_LISTEN:
        mouth_color = COL_LISTEN;
        a1 = 0;
        a2 = 359;                                   /* 小圆嘴 */
        mouth_w = 44;
        mouth_h = 30;
        mouth_top = MOUTH_TOP + 12;
        arc_w = 7;
        break;
    case FACE_THINK:
        eye = EYE_HALF;
        eye_top = EYE_TOP - 6;                      /* 眼睛往上看 */
        mouth_color = COL_MUTED;
        a1 = 40;
        a2 = 140;
        mouth_w = 42;
        mouth_h = 28;
        mouth_top = MOUTH_TOP + 14;
        arc_w = 7;
        break;
    case FACE_TALK:
        talk = true;                                /* 嘴巴动起来 */
        break;
    case FACE_SAD:
        eye = EYE_HALF;
        mouth_color = COL_WARN;
        a1 = 210;                                   /* 上弧：嘴角向下 */
        a2 = 330;
        break;
    case FACE_IDLE:
    default:
        break;
    }

    s_eye_scale = eye;
    lv_obj_set_style_transform_scale_x(s_eye_l, 256, 0);
    lv_obj_set_style_transform_scale_y(s_eye_l, eye, 0);
    lv_obj_set_style_transform_scale_x(s_eye_r, 256, 0);
    lv_obj_set_style_transform_scale_y(s_eye_r, eye, 0);
    lv_obj_align(s_eye_l, LV_ALIGN_TOP_MID, -EYE_DX, eye_top);
    lv_obj_align(s_eye_r, LV_ALIGN_TOP_MID, EYE_DX, eye_top);

    lv_obj_set_size(s_mouth, mouth_w, mouth_h);
    lv_obj_align(s_mouth, LV_ALIGN_TOP_MID, 0, mouth_top);
    lv_obj_set_style_transform_pivot_x(s_mouth, mouth_w / 2, 0);
    lv_obj_set_style_transform_pivot_y(s_mouth, mouth_h / 2, 0);
    lv_obj_set_style_transform_scale_y(s_mouth, 256, 0);
    lv_obj_set_style_arc_width(s_mouth, arc_w, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_mouth, lv_color_hex(mouth_color), LV_PART_MAIN);
    lv_arc_set_bg_angles(s_mouth, (uint32_t)a1, (uint32_t)a2);

    /* 呼吸：整张脸轻微缩放，让画面不至于死板 */
    anim_run(s_face, anim_scale_cb, 256, 264, 1700, true);

    if (talk) {
        anim_run(s_mouth, anim_scale_y_cb, 256, 340, 220, true);
    }
}

static face_mood_t mood_of_state(const char *state)
{
    if (state == NULL) {
        return FACE_WAIT;
    }
    if (strcmp(state, "IDLE") == 0) {
        return FACE_IDLE;
    }
    if (strcmp(state, "LISTENING") == 0) {
        return FACE_LISTEN;
    }
    if (strcmp(state, "THINKING") == 0) {
        return FACE_THINK;
    }
    if (strcmp(state, "SPEAKING") == 0) {
        return FACE_TALK;
    }
    if (strcmp(state, "ERROR") == 0 || strcmp(state, "DISCONNECTED") == 0 ||
            strcmp(state, "NO WIFI") == 0 || strcmp(state, "AUDIO ERROR") == 0 ||
            strcmp(state, "SERVER ERROR") == 0) {
        return FACE_SAD;
    }
    return FACE_WAIT;      /* BOOTING / WIFI / AUDIO / LINK / CONNECTING / CONNECTED */
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

    /*
     * 背景与 selector 一致：同一张 30° 斜向渐变图（components/ui_bg）。
     * 466×466 RGB565 生成在 PSRAM，所有页面共用；生成失败会退化为深色纯色。
     */
    ui_bg_apply(scr);

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
     * 中间的表情：替代原来的文字对话稿。
     * 不再渲染多轮中文正文，也就不会有 tiny_ttf 字形缓存吃光内部 RAM 的问题。
     */
    face_create(scr);

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
    face_set_mood(FACE_WAIT);

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
    face_set_mood(mood_of_state(state));

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
    (void)from_user;
    (void)text;

    /*
     * 不再显示文字稿：多轮中文正文会把 tiny_ttf 的字形缓存撑爆内部 RAM
     * （见 fonts_init 的说明）。表情由 xz_ui_set_state 驱动，这里只负责
     * 把"没听清"之类的提示清掉。
     */
    if (s_detail == NULL || bsp_display_lock(UI_LOCK_MS) != ESP_OK) {
        return;
    }
    set_text_if_changed(s_detail, "");
    bsp_display_unlock();
}

void xz_ui_clear_history(void)
{
    /* 没有文字稿可清了：表情跟着会话状态走，见 xz_ui_set_state */
}
