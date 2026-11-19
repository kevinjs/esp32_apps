/*
 * 最小 LVGL 界面：状态 + 一行详情 + 一个圆形按钮
 *
 * 466x466 圆屏，所有文本居中，避免溢出到圆角外。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef void (*xz_ui_click_cb_t)(void);

/** 启动显示并创建界面。cb 在 LVGL 任务上下文中被调用。 */
esp_err_t xz_ui_start(xz_ui_click_cb_t cb);

/** 顶部状态文本，例如 "IDLE" / "LISTENING" / "SPEAKING"。 */
void xz_ui_set_state(const char *state);

/** 单行提示（连接进度 / 错误 / "没听清" 等），传 NULL 清空。 */
void xz_ui_set_detail(const char *text);

/**
 * @brief 往可滚动对话区追加一条消息。
 *
 * 保留最近 12 条，超出自动删最旧的；追加后自动滚到最新一条，
 * 用户可触摸上下滑动回看。
 */
void xz_ui_add_message(bool from_user, const char *text);

/** 清空对话区（例如会话结束/断开时）。 */
void xz_ui_clear_history(void);

/** 显示激活码（未绑定时使用），传 NULL 隐藏。 */
void xz_ui_set_code(const char *code);
