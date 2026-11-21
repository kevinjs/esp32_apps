#include "ui_scroll_fx.h"

#include <stdint.h>
#include <string.h>

/* 用条目的 user_data 记住"这个条目的点击是本模块关掉的"，恢复时才知道要不要开回来 */
#define FX_DISABLED     ((void *)(intptr_t)1)

/** @brief 低于这个不透明度就当它已经看不见了，顺手禁掉点击 */
#define FADE_CLICK_OFF  40

/**
 * @brief 聚焦半径：离列表中心多少 px 以内开始放大
 *
 * 取 80 是为了比行间距略小一点——同一时间只有离中心最近的那一行接近
 * focus_scale，相邻行已经回落到 idle_scale，不会两行一起变大。
 */
#define FOCUS_SPAN      80

/* cfg == NULL 时用：不放大、不渐隐，只保留"滚出视野淡出" */
static const ui_scroll_fx_cfg_t DEFAULT_CFG = {
    .fade_top = 0,
    .fade_bottom = 0,
    .focus_scale = 256,
    .idle_scale = 256,
    .focus_opa = LV_OPA_COVER,
    .idle_opa = LV_OPA_COVER,
};

/**
 * @brief 条目落在容器可视区里的比例（按高度算）
 *
 * @return 256 = 完整可见，0 = 完全在视野外；定点整数，避免浮点
 */
static uint32_t visible_ratio(const lv_area_t *view, const lv_area_t *child)
{
    const int32_t height = child->y2 - child->y1 + 1;
    if (height <= 0) {
        return 256;
    }

    const int32_t top = (child->y1 > view->y1) ? child->y1 : view->y1;
    const int32_t bot = (child->y2 < view->y2) ? child->y2 : view->y2;
    const int32_t vis = (bot >= top) ? (bot - top + 1) : 0;

    return (uint32_t)(((int64_t)vis * 256) / height);
}

/** @brief 把 0..256 之外的值夹回来 */
static int32_t clamp256(int32_t v)
{
    if (v < 0) {
        return 0;
    }
    return (v > 256) ? 256 : v;
}

void ui_scroll_fx_refresh(lv_obj_t *list)
{
    if (list == NULL) {
        return;
    }

    /* 刚加完条目坐标还是旧的；列表本身没变时这里是空操作 */
    lv_obj_update_layout(list);

    const ui_scroll_fx_cfg_t *cfg = (const ui_scroll_fx_cfg_t *)lv_obj_get_user_data(list);
    if (cfg == NULL) {
        cfg = &DEFAULT_CFG;
    }

    lv_area_t view;
    lv_obj_get_coords(list, &view);
    const int32_t center = (view.y1 + view.y2) / 2;

    const uint32_t count = lv_obj_get_child_count(list);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *child = lv_obj_get_child(list, i);

        lv_area_t area;
        lv_obj_get_coords(child, &area);

        /* 1) 聚焦：离中心越近越大、越亮 */
        int32_t dist = (area.y1 + area.y2) / 2 - center;
        if (dist < 0) {
            dist = -dist;
        }
        const int32_t focus = clamp256(256 - dist * 256 / FOCUS_SPAN);

        int32_t scale = cfg->idle_scale +
                        (cfg->focus_scale - cfg->idle_scale) * focus / 256;
        int32_t opa = (int32_t)cfg->idle_opa +
                      ((int32_t)cfg->focus_opa - (int32_t)cfg->idle_opa) * focus / 256;

        /*
         * 2) 边缘渐隐：拿条目的"下边缘到上边界的距离"算，
         *    这样条目完整露出来时就是满亮度，只有真正滑出去的那一段才淡。
         */
        if (cfg->fade_top > 0) {
            opa = opa * clamp256((area.y2 - view.y1) * 256 / cfg->fade_top) / 256;
        }
        if (cfg->fade_bottom > 0) {
            opa = opa * clamp256((view.y2 - area.y1) * 256 / cfg->fade_bottom) / 256;
        }

        /* 3) 被容器边缘裁掉的部分同样按比例淡出 */
        opa = opa * (int32_t)visible_ratio(&view, &area) / 256;

        lv_obj_set_style_transform_scale_x(child, scale, 0);
        lv_obj_set_style_transform_scale_y(child, scale, 0);
        lv_obj_set_style_opa(child, (lv_opa_t)opa, 0);

        if (opa < FADE_CLICK_OFF) {
            if (lv_obj_is_clickable(child)) {
                lv_obj_set_clickable(child, false);
                lv_obj_set_user_data(child, FX_DISABLED);
            }
        } else if (lv_obj_get_user_data(child) == FX_DISABLED) {
            lv_obj_set_user_data(child, NULL);
            lv_obj_set_clickable(child, true);
        }
    }
}

static void on_scroll(lv_event_t *event)
{
    ui_scroll_fx_refresh((lv_obj_t *)lv_event_get_target(event));
}

static void on_delete(lv_event_t *event)
{
    ui_scroll_fx_cfg_t *cfg = (ui_scroll_fx_cfg_t *)lv_obj_get_user_data(
                                  (lv_obj_t *)lv_event_get_target(event));
    if (cfg != NULL) {
        lv_free(cfg);
    }
}

void ui_scroll_fx_attach(lv_obj_t *list, const ui_scroll_fx_cfg_t *cfg)
{
    if (list == NULL) {
        return;
    }

    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    lv_obj_add_event_cb(list, on_scroll, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(list, on_scroll, LV_EVENT_SCROLL_END, NULL);

    if (cfg != NULL) {
        ui_scroll_fx_cfg_t *copy = lv_malloc(sizeof(*copy));
        if (copy != NULL) {
            memcpy(copy, cfg, sizeof(*copy));
            lv_obj_set_user_data(list, copy);
            lv_obj_add_event_cb(list, on_delete, LV_EVENT_DELETE, NULL);
        }
    }

    ui_scroll_fx_refresh(list);
}
