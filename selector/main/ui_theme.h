/*
 * selector 的配色与通用样式（header-only）
 *
 * 所有 selector 页面（应用列表 / 设置 / Wi-Fi）共用同一套颜色，
 * 后面拆成 components/ui_kit 给各 app 复用时，从这里整体搬过去即可。
 */
#pragma once

#include "lvgl.h"
#include "ui_bg.h"      /* 斜向渐变背景已抽到 components/ui_bg，各 app 共用 */

#define UI_COLOR_BG_TOP     UI_BG_COLOR_TOP     /* 背景渐变：顶部近黑 */
#define UI_COLOR_BG_BOTTOM  UI_BG_COLOR_BOTTOM  /* 背景渐变：底部暗紫 */

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
