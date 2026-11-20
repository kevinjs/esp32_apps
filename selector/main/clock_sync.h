/*
 * 时钟：Wi-Fi 连上后走 SNTP 校时，供顶部状态栏显示 HH:MM
 *
 * 没有 RTC，断电后时间全靠每次开机同步；未同步时界面显示 "--:--"。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/** @brief 启动后台校时任务（不阻塞；Wi-Fi 未连上时最多等 30 s） */
void clock_sync_start(void);

/** @brief 已经拿到可信时间 */
bool clock_sync_valid(void);

/**
 * @brief 取本地时间 "HH:MM"
 *
 * @param buf 输出缓冲
 * @param len 缓冲长度（至少 6 字节）
 * @return true = 时间可信；false = 尚未同步，buf 里写的是 "--:--"
 */
bool clock_sync_hhmm(char *buf, size_t len);
