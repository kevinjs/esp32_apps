/*
 * selector 的配色与通用样式（header-only）
 *
 * 所有 selector 页面（应用列表 / 设置 / Wi-Fi）共用同一套颜色，
 * 后面拆成 components/ui_kit 给各 app 复用时，从这里整体搬过去即可。
 */
#pragma once

#include "lvgl.h"

#define UI_COLOR_BG_TOP     0x07070E   /* 背景渐变：顶部近黑 */
#define UI_COLOR_BG_BOTTOM  0x1A1230   /* 背景渐变：底部暗紫 */

#define UI_COLOR_TEXT       0xF2F2F2
#define UI_COLOR_MUTED      0x8A8A99
#define UI_COLOR_DIM        0x4A4A55

#define UI_COLOR_ROW        0x181826   /* 卡片底 */
#define UI_COLOR_ROW_EDGE   0x2A2A3A
#define UI_COLOR_TRACK      0x232333   /* 进度条轨道 */

#define UI_COLOR_OK         0x35D07F
#define UI_COLOR_WARN       0xE5B454
#define UI_COLOR_ERR        0xE74C3C
#define UI_COLOR_ACCENT     0x7C5CFF   /* 主色（紫） */
#define UI_COLOR_ACCENT_2   0x35D0FF   /* 主色渐变末端（青） */

/**
 * @brief 给整屏容器铺斜向渐变背景（实现在 ui_bg.c）
 *
 * LVGL 的 bg_grad_dir 只有竖直/水平两档，所以这里用 PSRAM 里生成的一张
 * 斜向渐变图当 bg_image，角度在 ui_bg.c 的 BG_TILT_DEG 调整。
 */
void ui_apply_gradient(lv_obj_t *obj);
