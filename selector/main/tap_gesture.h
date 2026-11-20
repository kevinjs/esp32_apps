/*
 * 点击手势判定（selector 内共用）
 *
 * 为什么不用 LV_EVENT_CLICKED：
 * LVGL 只判断按下/抬起是否落在同一对象上，手指拖着走几十像素后松手照样算点击。
 * 列表内容不满一屏时根本不会滚动，用户「想滑动」就会被当成点击 —— 在 selector
 * 里这等于把 app 装进 ota_0 并重启，代价太高。
 *
 * 用法：
 *   tap_gesture_attach(row);                                  // 内部挂 PRESSED / PRESS_LOST
 *   lv_obj_add_event_cb(row, cb, LV_EVENT_RELEASED, NULL);    // 自己的 RELEASED 回调
 *   cb 里：if (!tap_gesture_was_tap()) return;
 *
 * 单点触摸，全局只有一份状态，不需要按对象区分。
 */
#pragma once

#include <stdbool.h>

#include "lvgl.h"

/** @brief 给可点对象挂上按下位置跟踪（幂等：重复调用无副作用） */
void tap_gesture_attach(lv_obj_t *obj);

/** @brief 在 RELEASED 回调里调用；返回 true 表示这次是点击（并消费本次状态） */
bool tap_gesture_was_tap(void);
