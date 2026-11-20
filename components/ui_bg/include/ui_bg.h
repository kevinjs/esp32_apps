/*
 * 斜向渐变背景（selector 与各 app 共用）
 *
 * LVGL 的 bg_grad_dir 只有竖直/水平两档，给不了任意角度，所以这里在 PSRAM 里
 * 生成一张 466x466 RGB565 的渐变图当 bg_image 铺上去，一张图所有页面复用。
 *
 * 用法：
 *     ui_bg_apply(scr);      // 铺在整屏容器上，子对象保持透明即可看到渐变
 */
#pragma once

#include "lvgl.h"

#define UI_BG_COLOR_TOP     0x07070E    /* 渐变起点：顶部近黑 */
#define UI_BG_COLOR_BOTTOM  0x1A1230    /* 渐变终点：暗紫 */
#define UI_BG_TILT_DEG      30.0f       /* 渐变轴相对竖直方向的夹角（顺时针倾斜） */
#define UI_BG_SIZE          466         /* 生成图的边长（屏幕分辨率） */

/**
 * @brief 给对象铺斜向渐变背景（首次调用时生成图，之后复用）。
 *
 * 生成失败（PSRAM 不足）时退化为纯 UI_BG_COLOR_TOP 底色，不会崩。
 */
void ui_bg_apply(lv_obj_t *obj);
