#include "watch_stopwatch.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "watch.sw";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static bool    s_running;
static int64_t s_accumulated_us;   /* 本次运行前累计的时间 */
static int64_t s_started_us;       /* 本段运行的起点 */
static int64_t s_lap_base_us;      /* 上一段结束时的累计值 */
static int     s_lap_count;
static int64_t s_lap_split_us[WATCH_MAX_LAPS];
static int64_t s_lap_total_us[WATCH_MAX_LAPS];

static int64_t elapsed_locked(void)
{
    if (!s_running) {
        return s_accumulated_us;
    }
    return s_accumulated_us + (esp_timer_get_time() - s_started_us);
}

void watch_stopwatch_toggle(void)
{
    taskENTER_CRITICAL(&s_lock);
    if (s_running) {
        s_accumulated_us += esp_timer_get_time() - s_started_us;
        s_running = false;
    } else {
        s_started_us = esp_timer_get_time();
        s_running = true;
    }
    const bool running = s_running;
    const int64_t elapsed = elapsed_locked();
    taskEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "%s at %lld ms", running ? "start" : "pause", (long long)(elapsed / 1000));
}

void watch_stopwatch_reset(void)
{
    taskENTER_CRITICAL(&s_lock);
    if (s_running) {
        taskEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "reset ignored while running");
        return;
    }
    s_accumulated_us = 0;
    s_started_us = 0;
    s_lap_base_us = 0;
    s_lap_count = 0;
    memset(s_lap_split_us, 0, sizeof(s_lap_split_us));
    memset(s_lap_total_us, 0, sizeof(s_lap_total_us));
    taskEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "reset");
}

void watch_stopwatch_lap(void)
{
    taskENTER_CRITICAL(&s_lock);
    if (!s_running) {
        taskEXIT_CRITICAL(&s_lock);
        return;
    }

    const int64_t elapsed = elapsed_locked();
    const int64_t split = elapsed - s_lap_base_us;
    s_lap_base_us = elapsed;

    if (s_lap_count < WATCH_MAX_LAPS) {
        s_lap_split_us[s_lap_count] = split;
        s_lap_total_us[s_lap_count] = elapsed;
        s_lap_count++;
    } else {
        /* 满了就整体左移一格，保留最近 WATCH_MAX_LAPS 段 */
        memmove(s_lap_split_us, s_lap_split_us + 1, sizeof(s_lap_split_us) - sizeof(s_lap_split_us[0]));
        memmove(s_lap_total_us, s_lap_total_us + 1, sizeof(s_lap_total_us) - sizeof(s_lap_total_us[0]));
        s_lap_split_us[WATCH_MAX_LAPS - 1] = split;
        s_lap_total_us[WATCH_MAX_LAPS - 1] = elapsed;
    }
    const int count = s_lap_count;
    taskEXIT_CRITICAL(&s_lock);

    ESP_LOGI(TAG, "lap %d: %lld ms (total %lld ms)",
             count, (long long)(split / 1000), (long long)(elapsed / 1000));
}

void watch_stopwatch_snapshot(watch_stopwatch_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_lock);
    out->running = s_running;
    out->elapsed_us = elapsed_locked();
    out->lap_count = s_lap_count;
    memcpy(out->lap_split_us, s_lap_split_us, sizeof(out->lap_split_us));
    memcpy(out->lap_total_us, s_lap_total_us, sizeof(out->lap_total_us));
    taskEXIT_CRITICAL(&s_lock);
}
