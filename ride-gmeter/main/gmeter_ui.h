/*
 * 骑行加速度指示器 · 界面层（LVGL 9 / 466×466 圆形 AMOLED）
 *
 * 数据流：采样任务 100Hz 调 gmeter_ui_post() 写入快照，
 *         LVGL 任务里的 30fps 定时器读取快照刷新控件。
 * 这样 UI 刷新不受采样节奏影响，也不需要在采样任务里加 LVGL 锁。
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 送给界面的一帧数据 */
typedef struct {
    float lin[3];  /**< 线性加速度（m/s²，设备坐标系） */
    float mag;     /**< |lin| */
    float gyro[3]; /**< 已去零偏的角速度（°/s） */
    float pitch;   /**< 俯仰角（°） */
    float roll;    /**< 横滚角（°） */
    uint32_t ok;   /**< 成功采样计数 */
    uint32_t err;  /**< 失败采样计数 */
} gmeter_ui_data_t;

/** @brief 启动显示并创建界面（内部调用 bsp_display_start，会启动 LVGL 任务）。 */
esp_err_t gmeter_ui_start(void);

/** @brief 由采样任务调用，写入最新一帧数据（无锁，内部用临界区拷贝）。 */
void gmeter_ui_post(const gmeter_ui_data_t *data);

/** @brief 设置提示文字（如 CALIBRATING / READY）；UI 未创建时为空操作。 */
void gmeter_ui_set_hint(const char *text);

#ifdef __cplusplus
}
#endif
