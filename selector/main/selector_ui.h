/*
 * selector 界面：镜像列表 + 安装进度浮层。
 *
 * 所有函数都必须在持有 LVGL 锁（bsp_display_lock）时调用。
 */
#pragma once

#include <stddef.h>

#include "image_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*selector_select_cb_t)(const char *file);

/**
 * @brief 创建界面并启动状态轮询定时器。
 *
 * @param images   镜像数组（生命周期需覆盖整个 selector 运行期）
 * @param count    镜像数量
 * @param fs_total storage 总容量
 * @param fs_used  storage 已用容量
 * @param cb       用户选中某个镜像时回调（在 LVGL 任务上下文执行，不要阻塞）
 */
void selector_ui_create(
    const image_entry_t *images,
    int count,
    size_t fs_total,
    size_t fs_used,
    selector_select_cb_t cb
);

#ifdef __cplusplus
}
#endif
