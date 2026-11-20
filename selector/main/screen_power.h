/*
 * 屏幕开关（息屏 / 亮屏）
 *
 * 单击电源键（AXP2101 PWRKEY）切换：息屏时背光置 0 并禁用触摸输入，
 * 界面不响应任何点击，直到再次单击电源键。
 */
#pragma once

#include <stdbool.h>

/** @brief 记录当前亮度与触摸设备，需在 bsp_display_start() 之后调用 */
void screen_power_init(void);

/** @brief 切换息屏 / 亮屏 */
void screen_power_toggle(void);

/** @brief 当前是否处于息屏状态 */
bool screen_power_is_off(void);
