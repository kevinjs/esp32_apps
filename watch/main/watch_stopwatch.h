/*
 * 秒表状态机（与界面解耦，BOOT 键和屏幕按钮都调这里）
 *
 * 计时基准用 esp_timer_get_time()（微秒），不依赖系统时间，SNTP 校时跳变不影响计时。
 * 所有函数线程安全：BOOT 键任务与 LVGL 定时器都会调用。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WATCH_MAX_LAPS  5

typedef struct {
    bool    running;
    int64_t elapsed_us;                     /* 从开始到现在（暂停时停在累计值） */
    int     lap_count;                      /* 已记录的段数（最多 WATCH_MAX_LAPS） */
    int64_t lap_split_us[WATCH_MAX_LAPS];   /* 每段用时 */
    int64_t lap_total_us[WATCH_MAX_LAPS];   /* 该段结束时的总时间 */
} watch_stopwatch_snapshot_t;

/** @brief 开始 / 暂停（BOOT 短按与屏幕按钮共用） */
void watch_stopwatch_toggle(void);

/** @brief 复位；运行中调用会被忽略（先暂停再复位） */
void watch_stopwatch_reset(void);

/** @brief 记一段；仅在运行中有效 */
void watch_stopwatch_lap(void);

/** @brief 读快照，供 UI 渲染 */
void watch_stopwatch_snapshot(watch_stopwatch_snapshot_t *out);
