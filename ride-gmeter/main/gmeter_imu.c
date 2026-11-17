#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"

/* BSP 只用来拿 I2C 总线句柄（bsp_i2c_get_handle 内部会按需初始化） */
#include "bsp/esp-bsp.h"

/*
 * qmi8658.h 无条件 #define M_PI，与 IDF v6 (picolibc) 的 math.h 冲突，
 * 官方示例 04_Immersive_block 也用同样的办法绕开。
 */
#ifdef M_PI
#undef M_PI
#endif
#include "qmi8658.h"

#include "gmeter_config.h"
#include "gmeter_imu.h"

static const char *TAG = "gmeter.imu";

/**
 * 板载 QMI8658 的 7 位 I2C 地址：官方示例用 QMI8658_ADDRESS_HIGH(0x6B)。
 * 不同板卡 SA0 接法可能不同，这里按 0x6B → 0x6A 顺序探测，避免烧错了还要改代码重刷。
 */
static const uint8_t s_addr_candidates[] = {0x6B, 0x6A};

static qmi8658_dev_t s_dev;
static uint8_t s_addr_used = 0;
static float s_gyro_bias[3] = {0};  /* 静止标定得到的陀螺零偏（°/s） */
static bool s_inited = false;

esp_err_t gmeter_imu_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_FAIL, TAG, "I2C bus is not available");

    esp_err_t err = ESP_FAIL;
    for (size_t i = 0; i < sizeof(s_addr_candidates); i++) {
        err = qmi8658_init(&s_dev, bus, s_addr_candidates[i]);
        if (err == ESP_OK) {
            s_addr_used = s_addr_candidates[i];
            break;
        }
        ESP_LOGW(TAG, "no response at 0x%02X (%s)", s_addr_candidates[i], esp_err_to_name(err));
    }
    ESP_RETURN_ON_ERROR(err, TAG, "qmi8658 not found at 0x6B/0x6A, check I2C (SDA=15 SCL=14)");

    uint8_t who = 0;
    if (qmi8658_get_who_am_i(&s_dev, &who) == ESP_OK) {
        ESP_LOGI(TAG, "QMI8658 detected at 0x%02X, WHO_AM_I=0x%02X", s_addr_used, who);
    }

    /* 加速度：±8G / 500Hz，单位 m/s² */
    ESP_RETURN_ON_ERROR(qmi8658_set_accel_range(&s_dev, QMI8658_ACCEL_RANGE_8G), TAG,
                        "set accel range failed");
    ESP_RETURN_ON_ERROR(qmi8658_set_accel_odr(&s_dev, QMI8658_ACCEL_ODR_500HZ), TAG,
                        "set accel odr failed");
    qmi8658_set_accel_unit_mps2(&s_dev, true);

    /* 陀螺：骑行压弯约 ±45°，256dps 量程足够且分辨率更好 */
    ESP_RETURN_ON_ERROR(qmi8658_set_gyro_range(&s_dev, QMI8658_GYRO_RANGE_256DPS), TAG,
                        "set gyro range failed");
    ESP_RETURN_ON_ERROR(qmi8658_set_gyro_odr(&s_dev, QMI8658_GYRO_ODR_500HZ), TAG,
                        "set gyro odr failed");
    qmi8658_set_gyro_unit_dps(&s_dev, true);

    ESP_RETURN_ON_ERROR(qmi8658_enable_sensors(&s_dev, QMI8658_ENABLE_ACCEL | QMI8658_ENABLE_GYRO),
                        TAG, "enable sensors failed");

    s_inited = true;
    ESP_LOGI(TAG, "IMU ready @0x%02X: accel 8G/500Hz, gyro 256dps/500Hz", s_addr_used);
    return ESP_OK;
}

esp_err_t gmeter_imu_read(float accel_mps2[3], float gyro_dps[3])
{
    ESP_RETURN_ON_FALSE(s_inited, ESP_ERR_INVALID_STATE, TAG, "IMU not initialized");

    qmi8658_data_t data = {0};
    ESP_RETURN_ON_ERROR(qmi8658_read_sensor_data(&s_dev, &data), TAG, "read sensor failed");

    if (accel_mps2 != NULL) {
        accel_mps2[0] = data.accelX;
        accel_mps2[1] = data.accelY;
        accel_mps2[2] = data.accelZ;
    }
    if (gyro_dps != NULL) {
        gyro_dps[0] = data.gyroX - s_gyro_bias[0];
        gyro_dps[1] = data.gyroY - s_gyro_bias[1];
        gyro_dps[2] = data.gyroZ - s_gyro_bias[2];
    }
    return ESP_OK;
}

esp_err_t gmeter_imu_calibrate(float accel_bias[3], float gyro_bias[3],
                               int samples, int period_ms)
{
    ESP_RETURN_ON_FALSE(s_inited, ESP_ERR_INVALID_STATE, TAG, "IMU not initialized");
    ESP_RETURN_ON_FALSE(samples > 0, ESP_ERR_INVALID_ARG, TAG, "invalid samples");

    float sum[3] = {0};
    float gsum[3] = {0};
    int valid = 0;

    for (int i = 0; i < samples; i++) {
        /*
         * 注意：组件 v2.0.2 的头文件里声明了 qmi8658_read_accel_mps2 / read_gyro_dps，
         * 但 .c 里并没有实现（链接会 undefined reference）。这里统一走 read_sensor_data，
         * 单位在 init 时已设置为 m/s² / dps。
         */
        qmi8658_data_t data = {0};
        if (qmi8658_read_sensor_data(&s_dev, &data) == ESP_OK) {
            sum[0] += data.accelX;
            sum[1] += data.accelY;
            sum[2] += data.accelZ;
            gsum[0] += data.gyroX;
            gsum[1] += data.gyroY;
            gsum[2] += data.gyroZ;
            valid++;
        }
        vTaskDelay(pdMS_TO_TICKS(period_ms));
    }

    ESP_RETURN_ON_FALSE(valid >= samples / 2, ESP_FAIL, TAG,
                        "too few samples during calibration (%d/%d)", valid, samples);

    float mean[3];
    for (int i = 0; i < 3; i++) {
        mean[i] = sum[i] / (float)valid;
    }

    const float mag = sqrtf(mean[0] * mean[0] + mean[1] * mean[1] + mean[2] * mean[2]);
    ESP_RETURN_ON_FALSE(mag > 1.0f, ESP_FAIL, TAG, "invalid gravity magnitude %.3f", mag);

    if (fabsf(mag - 9.80665f) > GMETER_CALIB_MAG_TOLERANCE) {
        ESP_LOGW(TAG, "|g|=%.2f m/s^2 deviates from 9.81, device may have been moving", mag);
    }

    /* 加速度计：只能修正沿重力方向的分量 */
    const float scale = 9.80665f / mag;
    for (int i = 0; i < 3; i++) {
        if (accel_bias != NULL) {
            accel_bias[i] = mean[i] - mean[i] * scale;
        }
    }

    /* 陀螺仪：静止时理论输出为 0，均值即零偏，直接存进驱动并立即生效 */
    for (int i = 0; i < 3; i++) {
        s_gyro_bias[i] = gsum[i] / (float)valid;
        if (gyro_bias != NULL) {
            gyro_bias[i] = s_gyro_bias[i];
        }
    }

    ESP_LOGI(TAG, "calibration done (%d samples): |g|=%.3f, gyro bias=[%.2f %.2f %.2f] dps",
             valid, mag, s_gyro_bias[0], s_gyro_bias[1], s_gyro_bias[2]);
    return ESP_OK;
}

void gmeter_imu_deinit(void)
{
    if (s_inited) {
        qmi8658_enable_accel(&s_dev, false);
        qmi8658_enable_gyro(&s_dev, false);
        s_inited = false;
    }
}
