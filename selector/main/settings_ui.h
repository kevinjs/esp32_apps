/*
 * 设置页（selector 内，与应用平级的入口）
 *
 * 现在只有「Wi-Fi 设置」一项，点进去复用 wifi_setup_open()。
 * 后续加显示/时间/关于等设置项时，往这里的列表里加行即可。
 */
#pragma once

/**
 * @brief 打开设置页（页面只创建一次，重复调用只是显示并置顶）
 *
 * @note 必须在已持有 LVGL 锁的上下文中调用（LVGL 事件回调里即可）。
 */
void settings_open(void);
