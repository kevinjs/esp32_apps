/*
 * watch 可调参数集中在这里。
 *
 * 交互约定（只有一个物理 BOOT 键，其余靠触摸）：
 *   BOOT 短按（< TAP_MAX_MS）        ：开始 / 暂停，并切到秒表页
 *   BOOT 长按（TAP_MAX ~ 2900 ms）  ：记一段（仅在秒表运行中有效）
 *   BOOT 按住 3000 ms                ：由 boot_switch 接管，回 selector
 *   左右滑动                          ：表盘 ⇄ 秒表
 *
 * 复位只在屏幕上做：运行中按复位没有意义（秒表模块会忽略并打日志），
 * 这样不会出现"想暂停却把成绩抹掉"的情况。
 */
#pragma once

/* ------------------------------ 按键 ------------------------------ */

/** BOOT 键 GPIO（本板 BOOT = GPIO0，低电平有效） */
#define WATCH_BOOT_GPIO             0

/** 消抖时间（ms）：短于这个时长视为抖动，忽略 */
#define WATCH_BOOT_DEBOUNCE_MS      50

/** 短按上限（ms）：短于它算"开始/暂停" */
#define WATCH_BOOT_TAP_MAX_MS       600

/** 中等按住的上限（ms）：短按上限 ~ 它之间算"记圈" */
#define WATCH_BOOT_LAP_MAX_MS       2900

/** 长按回 selector 的时间（ms），必须与 boot_switch 的配置一致 */
#define WATCH_BOOT_SELECTOR_MS      3000

/** 按键轮询周期（ms） */
#define WATCH_KEY_POLL_MS           20

/* ------------------------------ 界面 ------------------------------ */

/** 界面刷新周期（ms）：秒表按百分秒显示，10Hz 已经够用且省电 */
#define WATCH_TICK_MS               100

#define WATCH_COL_TEXT              0xF2F2F2    /* 与 selector 的 UI_COLOR_TEXT 一致 */
#define WATCH_COL_DIM               0x8A8A99    /* 与 selector 的 UI_COLOR_MUTED 一致 */
#define WATCH_COL_IDLE              0x4A4A55    /* 与 selector 的 UI_COLOR_DIM 一致 */
#define WATCH_COL_ACCENT            0x35D0FF    /* 与 selector 的主色末端（青）一致 */
#define WATCH_COL_OK                0x35D07F
#define WATCH_COL_WARN              0xE5B454
