/*
 * 圆屏纵向列表的"聚焦放大 + 边缘渐隐"效果（selector / 各 app 共用）
 *
 * 用法：
 *     static const ui_scroll_fx_cfg_t MENU_FX = {
 *         .fade_top    = 56,
 *         .fade_bottom = 48,
 *         .focus_scale = 320,   // 正对中心的行 125%
 *         .idle_scale  = 220,   // 其它行 86%
 *         .focus_opa   = LV_OPA_COVER,
 *         .idle_opa    = 225,
 *     };
 *     ui_scroll_fx_attach(list, &MENU_FX);   // cfg 内部会拷贝一份
 *     ui_scroll_fx_refresh(list);            // 增删条目后刷一次
 *
 * 拖动、惯性、回弹都还是 LVGL 自带的滚动，这里只做视觉：
 *   - 关掉滚动条
 *   - 离列表竖直中心最近的行放大（focus_*），越远越接近 idle_*
 *   - 条目滑到上下边缘时按 fade_top / fade_bottom 渐隐，不会被硬裁一刀
 *   - 快看不见的条目顺手关掉点击，避免摸到边缘误触发
 *
 * 渐隐用的是"条目边缘进入渐隐带多少"，不是"离中心多远"，
 * 所以列表停住时首行只要完整露出来就是 100%，不会长期发灰。
 *
 * 注意：条目必须是容器的直接子对象，容器的 flex 布局不要用"居中拉伸"，
 * 否则缩放会互相顶开（本仓库的菜单都是 flex column + START 对齐，满足条件）。
 * 放大后的宽度不能超过容器宽度，否则会被容器裁掉，所以列表要适当放宽。
 *
 * 条目自己的 user_data 会被本模块占用（标记"点击是它关掉的"），列表的条目不要再拿来放别的。
 */
#pragma once

#include "lvgl.h"

/** @brief 列表滚动效果参数；数值都是 LVGL 的定点比例（256 = 100%） */
typedef struct {
    int32_t fade_top;       /**< 顶部渐隐带高度（px），0 = 不渐隐 */
    int32_t fade_bottom;    /**< 底部渐隐带高度（px），0 = 不渐隐 */
    int32_t focus_scale;    /**< 正对中心的行缩放，256 = 100% */
    int32_t idle_scale;     /**< 远离中心的行缩放 */
    lv_opa_t focus_opa;     /**< 正对中心的行不透明度 */
    lv_opa_t idle_opa;      /**< 远离中心的行不透明度 */
} ui_scroll_fx_cfg_t;

/**
 * @brief 给纵向列表挂上"聚焦放大 + 边缘渐隐"效果
 *
 * @param list 竖直可滚动的容器
 * @param cfg  参数（内部拷贝一份，调用方可以传栈上临时变量）；NULL = 只做滚出视野淡出
 */
void ui_scroll_fx_attach(lv_obj_t *list, const ui_scroll_fx_cfg_t *cfg);

/** @brief 立即按当前滚动位置刷一遍（重建条目之后必须调用） */
void ui_scroll_fx_refresh(lv_obj_t *list);
