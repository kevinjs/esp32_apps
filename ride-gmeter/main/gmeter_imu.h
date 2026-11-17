/*
 * QMI8658 采样封装。
 *
 * 板载 IMU 与触摸/音频/PMIC 共用 I2C（SDA=GPIO15, SCL=GPIO14, 7 位地址 0x6B），
 * 这里复用 BSP 的 I2C 总线句柄，不重复创建。
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 I2C 总线与 QMI8658（±8G / 500Hz，陀螺 256dps / 500Hz）。
 */
esp_err_t gmeter_imu_init(void);

/**
 * @brief 读取一帧数据。
 *
 * @param accel_mps2 加速度（m/s²，含重力），可为 NULL
 * @param gyro_dps   角速度（°/s），可为 NULL
 */
esp_err_t gmeter_imu_read(float accel_mps2[3], float gyro_dps[3]);

/**
 * @brief 静止零偏标定（加速度计 + 陀螺仪）。
 *
 * 加速度计：静止时真实加速度只有重力，只能标定「沿重力方向」的那部分零偏：
 *   accel_bias = mean(raw) - normalize(mean(raw)) * 9.80665
 * 垂直于重力方向的分量在静止时不可观测，需要绕轴翻转才能测出，本项目不做。
 *
 * 陀螺仪：静止时理想输出为 0，直接取均值即为零偏；标定结果保存在驱动内部，
 * 后续 gmeter_imu_read() 返回的角速度已自动扣除。
 *
 * @param accel_bias   输出：加速度计三轴零偏（m/s²），可为 NULL
 * @param gyro_bias    输出：陀螺仪三轴零偏（°/s），可为 NULL
 * @param samples      采样数
 * @param period_ms    采样间隔（ms）
 */
esp_err_t gmeter_imu_calibrate(float accel_bias[3], float gyro_bias[3],
                               int samples, int period_ms);

/** @brief 释放驱动（目前仅用于错误回滚，可重复调用）。 */
void gmeter_imu_deinit(void);

#ifdef __cplusplus
}
#endif
