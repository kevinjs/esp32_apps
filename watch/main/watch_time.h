/*
 * 怀表时间源
 *
 * 板上没有 RTC，时间来源只有两条：
 *   1) 开机先从 NVS 恢复上次同步的 UTC（让表针立刻指到大概位置）
 *   2) Wi-Fi 连上后用 SNTP 校时，成功后写回 NVS
 *
 * 时区固定 CST-8（东八区）；以后要可配置时挪到 app_settings。
 */
#pragma once

#include <stdbool.h>
#include <time.h>

/** @brief 恢复 NVS 时间并起后台校时任务（不阻塞） */
void watch_time_start(void);

/**
 * @brief 取本地时间
 *
 * @param out 输出（会填 tm_hour/min/sec/mday/mon/wday，可直接用于表盘）
 * @return true = 时间可信；false = 还没校过时（表盘仍会画，只是从 1970 起算）
 */
bool watch_time_get(struct tm *out);
