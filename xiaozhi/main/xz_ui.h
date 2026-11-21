/*
 * 最小 LVGL 界面：状态 + 一张会动的表情 + 一行详情 + 一个圆形按钮
 *
 * 466x466 圆屏，所有文本居中，避免溢出到圆角外。
 *
 * 不显示多轮对话的文字稿：中文正文每一轮都会给 tiny_ttf 增加字形缓存，
 * 十几轮就能把内部 RAM 吃干（进而让 esp-aes 解密分配失败、会话卡死）。
 * 对话过程改用表情表达（等 / 听 / 想 / 说 / 出错）。
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
 * @brief 收到一轮识别/播报文本时的通知（不再渲染文字）。
 *
 * 界面已经改成表情表达，这里只用于清掉"没听清"之类的提示，
 * 传进来的文本只打日志，不上屏。
 */
void xz_ui_add_message(bool from_user, const char *text);

/** 保留接口：表情跟着会话状态走，无需单独清历史。 */
void xz_ui_clear_history(void);

/** 显示激活码（未绑定时使用），传 NULL 隐藏。 */
void xz_ui_set_code(const char *code);
