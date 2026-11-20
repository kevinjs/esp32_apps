/*
 * 怀表界面：圆形表盘 + 秒表页，左右滑动切换。
 *
 * 界面只做两件事：把 watch_time / watch_stopwatch 的快照画出来，
 * 以及把点击转成 watch_stopwatch_* 的调用。所有业务状态都在模块里，
 * 界面不保存秒表状态，重启界面不会丢数据。
 */
#pragma once

#include "esp_err.h"

/** 页序号（也可直接当滑动索引传给 watch_ui_show_page） */
#define WATCH_PAGE_FACE         0
#define WATCH_PAGE_STOPWATCH    1

/**
 * @brief 初始化显示并构建界面（内部调用 bsp_display_start + cjk_font_init）。
 *
 * 会创建 100ms 的 LVGL 定时器刷新时间与秒表。必须在 app_main 里调用一次。
 */
esp_err_t watch_ui_start(void);

/** @brief 切到指定页（带滑动动画）；可在任意任务里调用 */
void watch_ui_show_page(int page);

/** @brief 切到秒表页（BOOT 键按下时用） */
void watch_ui_show_stopwatch(void);
