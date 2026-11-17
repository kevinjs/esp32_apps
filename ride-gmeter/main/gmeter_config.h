/*
 * ride-gmeter 可调参数集中在这里。
 *
 * 阶段一（当前）：只做 IMU 采样 + 去重力 + 滤波，通过串口观察数据质量。
 * 所有数值都是初始值，实测后按注释里的经验范围再调。
 */
#pragma once

/* ---------------- 采样 ---------------- */

/** 采样任务周期（ms）。100Hz 足够：骑行真实动作 < 2Hz，车把振动由滤波器处理 */
#define GMETER_SAMPLE_PERIOD_MS     10

/** 串口打印周期（ms）。50Hz 对串口绘图足够，再高会淹没日志 */
#define GMETER_PRINT_PERIOD_MS      20

/* ---------------- 滤波器 ---------------- */

/**
 * 重力估计低通时间常数（s）。
 * 0.3~1.0 之间取舍：太小会把真实加减速当重力吃掉，太大会让重力估计跟不上姿态变化。
 */
#define GMETER_GRAVITY_TAU_S        0.5f

/** 线性加速度二阶低通截止频率（Hz）。5Hz 保留加减速/压弯，滤掉车把振动 */
#define GMETER_LP_CUTOFF_HZ         5.0f

/** 死区（m/s²）。小于该值视为 0，避免静止时数字乱跳 */
#define GMETER_DEADBAND_MPS2        0.15f

/* ---------------- 标定 ---------------- */

/** 静止标定采样数（200 × 10ms = 2 秒） */
#define GMETER_CALIB_SAMPLES        200

/** 标定前等待时间（ms），让手离开设备 */
#define GMETER_CALIB_SETTLE_MS      800

/** 标定可信度阈值：静止时 |g| 应接近 9.8 m/s²，偏离超过该值视为“设备在动” */
#define GMETER_CALIB_MAG_TOLERANCE  1.5f

/* ---------------- 姿态门控 ---------------- */

/**
 * 陀螺合角速度门控阈值（°/s）。
 * 手持晃动/快速翻转时重力估计跟不上姿态变化，会产生大量"假加速度"，
 * 超过该阈值就把线性加速度输出去权重（约 100ms 内衰减到 0），避免乱闪。
 * 骑行时陀螺通常 < 30°/s，不受影响。
 */
#define GMETER_GYRO_GATE_DPS        120.0f

/* ---------------- 界面 ---------------- */

/** 气泡满量程（m/s²）：6 对应气泡到达最外圈 */
#define GMETER_UI_FULL_SCALE_MPS2   6.0f

/** 状态判定阈值（m/s²）：超过才显示 ACCEL / BRAKE，压掉踏频级别的波动 */
#define GMETER_UI_STATE_MPS2        1.2f

/** 气泡变黄阈值（瞬时） */
#define GMETER_UI_YELLOW_MPS2       3.0f

/** 气泡变红阈值（需持续 GMETER_UI_RED_HOLD_MS，避免颠簸脉冲误报） */
#define GMETER_UI_RED_MPS2          5.0f
#define GMETER_UI_RED_HOLD_MS       200

/** 垂直/路面通道使用的轴（0=X,1=Y,2=Z）与其满量程（m/s²） */
#define GMETER_UI_AXIS_VERT         2
#define GMETER_UI_ROAD_FULL_MPS2    8.0f

/** 每 1 m/s² 对应的像素位移（140 px / 6 m/s² ≈ 23） */
#define GMETER_UI_SCALE_PX_PER_MPS2 23.0f

/**
 * 轴映射。重力只能确定垂直方向，装到车把上的“车头方向”无法自动识别，
 * 因此这里给出可调映射：装反了就把 SIGN 改成相反值。
 */
#define GMETER_UI_AXIS_LONG         0   /* 纵向（前后）使用 accel X */
#define GMETER_UI_AXIS_LAT          1   /* 横向（左右）使用 accel Y */
#define GMETER_UI_LONG_SIGN         (-1.0f) /* 前进方向对应气泡向上 */
#define GMETER_UI_LAT_SIGN          (1.0f)

/* ---------------- 按键 ---------------- */

/** BOOT 键 GPIO（本板 BOOT = GPIO0，低电平有效） */
#define GMETER_BUTTON_GPIO          0

/** 按键消抖时间（ms） */
#define GMETER_BUTTON_DEBOUNCE_MS   50

/** 短按松开的最长按压时间（ms）：超过这个时长不再触发重标定 */
#define GMETER_BUTTON_RECAL_MAX_HOLD_MS     1000

/** 长按回到 selector 的时间（ms），由 boot_switch 组件接管 */
#define GMETER_BUTTON_SELECTOR_HOLD_MS      3000
