#include <math.h>
#include <string.h>

#include "gmeter_filter.h"

#define GMETER_PI 3.14159265358979f
#define GMETER_G  9.80665f

static inline float alpha_from_tau(float dt_s, float tau_s)
{
    if (tau_s <= 0.0f) {
        return 1.0f;
    }
    return dt_s / (tau_s + dt_s);
}

static inline float alpha_from_cutoff(float dt_s, float cutoff_hz)
{
    if (cutoff_hz <= 0.0f) {
        return 1.0f;
    }
    const float rc = 1.0f / (2.0f * GMETER_PI * cutoff_hz);
    return dt_s / (rc + dt_s);
}

void gmeter_filter_init(gmeter_filter_t *f, float dt_s, float gravity_tau_s,
                        float lp_cutoff_hz, float deadband)
{
    if (f == NULL) {
        return;
    }

    memset(f, 0, sizeof(*f));
    f->alpha_gravity = alpha_from_tau(dt_s, gravity_tau_s);
    f->alpha_lp = alpha_from_cutoff(dt_s, lp_cutoff_hz);
    f->deadband = deadband;
    f->seeded = false;
}

void gmeter_filter_set_bias(gmeter_filter_t *f, const float bias[3])
{
    if (f == NULL || bias == NULL) {
        return;
    }
    memcpy(f->bias, bias, sizeof(f->bias));
}

void gmeter_filter_reset_state(gmeter_filter_t *f)
{
    if (f == NULL) {
        return;
    }
    memset(f->gravity, 0, sizeof(f->gravity));
    memset(f->lp1, 0, sizeof(f->lp1));
    memset(f->lp2, 0, sizeof(f->lp2));
    memset(f->lin, 0, sizeof(f->lin));
    f->seeded = false;
}

void gmeter_filter_update(gmeter_filter_t *f, const float accel_raw[3], float dt_s,
                          float lin_out[3])
{
    if (f == NULL || accel_raw == NULL) {
        return;
    }
    if (dt_s <= 0.0f || dt_s > 1.0f) {
        dt_s = 0.01f; /* 异常 dt（首帧/被抢占）时退回名义周期，避免滤波器发散 */
    }

    for (int i = 0; i < 3; i++) {
        const float v = accel_raw[i] - f->bias[i];

        if (!f->seeded) {
            f->gravity[i] = v;
            f->lp1[i] = 0.0f;
            f->lp2[i] = 0.0f;
        }

        /* 1) 重力：长时间常数低通 */
        f->gravity[i] += f->alpha_gravity * (v - f->gravity[i]);

        /* 2) 线性加速度 = 原始 - 重力 */
        const float lin = v - f->gravity[i];

        /* 3) 两级一阶低通级联 = 二阶低通（比单级更陡，且无相位突跳） */
        f->lp1[i] += f->alpha_lp * (lin - f->lp1[i]);
        f->lp2[i] += f->alpha_lp * (f->lp1[i] - f->lp2[i]);

        /* 4) 死区 */
        f->lin[i] = (fabsf(f->lp2[i]) < f->deadband) ? 0.0f : f->lp2[i];

        if (lin_out != NULL) {
            lin_out[i] = f->lin[i];
        }
    }

    f->seeded = true;
}

void gmeter_filter_tilt_deg(const gmeter_filter_t *f, float *pitch_deg, float *roll_deg)
{
    if (f == NULL) {
        return;
    }

    /* 以重力向量为准：pitch 为前后俯仰，roll 为左右横滚 */
    if (pitch_deg != NULL) {
        *pitch_deg = atan2f(-f->gravity[0], sqrtf(f->gravity[1] * f->gravity[1] +
                                                f->gravity[2] * f->gravity[2])) *
                     180.0f / GMETER_PI;
    }
    if (roll_deg != NULL) {
        *roll_deg = atan2f(f->gravity[1], f->gravity[2]) * 180.0f / GMETER_PI;
    }
}

float gmeter_norm3(const float v[3])
{
    if (v == NULL) {
        return 0.0f;
    }
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}
