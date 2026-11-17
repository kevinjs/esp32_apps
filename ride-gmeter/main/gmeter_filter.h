/*
 * 加速度处理：去重力 + 低通 + 死区。
 *
 * 加速度计输出的原始值 = 重力 + 运动加速度。骑行时真正关心的是后者，
 * 因此先用长时间常数低通估计重力向量，再从原始值里减掉。
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float gravity[3];    /**< 估计的重力向量（设备坐标系，m/s²） */
    float lp1[3];        /**< 一阶低通第一级状态 */
    float lp2[3];        /**< 一阶低通第二级状态（两级级联构成二阶） */
    float bias[3];       /**< 传感器零偏（m/s²），由静止标定得到 */
    float lin[3];        /**< 最近一次的线性加速度输出（m/s²） */
    float alpha_gravity; /**< 重力估计低通系数 */
    float alpha_lp;      /**< 线性加速度低通系数 */
    float deadband;      /**< 死区（m/s²） */
    bool seeded;         /**< 重力估计是否已用首个样本初始化 */
} gmeter_filter_t;

/**
 * @brief 初始化滤波器。
 *
 * @param f              滤波器实例
 * @param dt_s           采样周期（s），例如 0.01
 * @param gravity_tau_s  重力估计时间常数（s）
 * @param lp_cutoff_hz   线性加速度低通截止频率（Hz）
 * @param deadband       死区（m/s²）
 */
void gmeter_filter_init(gmeter_filter_t *f, float dt_s, float gravity_tau_s,
                        float lp_cutoff_hz, float deadband);

/** @brief 设置零偏（通常在静止标定后调用）。 */
void gmeter_filter_set_bias(gmeter_filter_t *f, const float bias[3]);

/** @brief 清空内部状态（重力/低通），下一次 update 会重新用新样本seed。 */
void gmeter_filter_reset_state(gmeter_filter_t *f);

/**
 * @brief 单步更新。
 *
 * @param f          滤波器实例
 * @param accel_raw  原始加速度（m/s²，含重力）
 * @param dt_s       本次实际采样间隔（s），建议用实测值而不是名义值
 * @param lin_out    输出：去重力 + 低通 + 死区后的线性加速度（m/s²），可为 NULL
 */
void gmeter_filter_update(gmeter_filter_t *f, const float accel_raw[3], float dt_s,
                          float lin_out[3]);

/** @brief 由重力估计推算俯仰/横滚角（度）。安装朝向不同，轴映射可能相反。 */
void gmeter_filter_tilt_deg(const gmeter_filter_t *f, float *pitch_deg, float *roll_deg);

/** @brief 三轴向量模长。 */
float gmeter_norm3(const float v[3]);

#ifdef __cplusplus
}
#endif
