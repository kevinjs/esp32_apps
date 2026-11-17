/*
 * ride-gmeter · 阶段一：线性加速度探测
 *
 * 目标不是做 UI，而是先确认「去重力 + 滤波」后的数据质量是否可用于骑行场景。
 * 串口输出两条流：
 *   1) 50Hz CSV：t_ms, ax, ay, az, |a|, gx, gy, gz
 *      （ax/ay/az 是**去重力后**的线性加速度，单位 m/s²）
 *   2) 1Hz 注释行（以 '#' 开头，不会污染串口绘图）：重力向量与俯仰/横滚角
 *
 * BOOT 键（GPIO0）按下 = 重新做一次静止标定。
 *
 * 验证方法见 README：静止 / 直线加速 / 急刹 / 压弯 四组动作。
 */
#include <math.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "gmeter_config.h"
#include "gmeter_filter.h"
#include "gmeter_imu.h"
#include "gmeter_ui.h"

static const char *TAG = "ride-gmeter";

static gmeter_filter_t s_filter;
static volatile bool s_recal_request = false;

/* -------------------------------------------------------------------------- */
/*  BOOT 键：重新标定                                                          */
/* -------------------------------------------------------------------------- */

static void button_init(void)
{
    /* 不用 GPIO 中断：BSP 的触摸初始化已经占用了 ISR 服务，
     * 而且采样任务本来就是 10ms 周期，轮询足够且不会污染日志。 */
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << GMETER_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_LOGI(TAG, "BOOT (GPIO%d) = recalibrate", GMETER_BUTTON_GPIO);
}

/** @brief 轮询 BOOT 键，检测到下降沿（带消抖）时置位重标定请求。 */
static void button_poll(int64_t now_us)
{
    static int last_level = 1;
    static int64_t last_press_us = 0;

    const int level = gpio_get_level(GMETER_BUTTON_GPIO);
    if (level == 0 && last_level == 1 &&
        (now_us - last_press_us) > (int64_t)GMETER_BUTTON_DEBOUNCE_MS * 1000) {
        last_press_us = now_us;
        s_recal_request = true;
    }
    last_level = level;
}

/* -------------------------------------------------------------------------- */
/*  静止标定                                                                   */
/* -------------------------------------------------------------------------- */

static esp_err_t run_calibration(const char *reason)
{
    float accel_bias[3] = {0};
    float gyro_bias[3] = {0};
    const int duration_ms = GMETER_CALIB_SAMPLES * GMETER_SAMPLE_PERIOD_MS;

    ESP_LOGW(TAG, "calibrating (%s): keep the device still for %d ms ...", reason, duration_ms);
    gmeter_ui_set_hint("CALIBRATING");
    vTaskDelay(pdMS_TO_TICKS(GMETER_CALIB_SETTLE_MS));

    esp_err_t err = gmeter_imu_calibrate(accel_bias, gyro_bias,
                                         GMETER_CALIB_SAMPLES, GMETER_SAMPLE_PERIOD_MS);
    gmeter_ui_set_hint("READY");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "calibration failed: %s", esp_err_to_name(err));
        return err;
    }

    gmeter_filter_set_bias(&s_filter, accel_bias);
    gmeter_filter_reset_state(&s_filter);

    ESP_LOGI(TAG, "accel bias = [%.3f %.3f %.3f] m/s^2, gyro bias = [%.2f %.2f %.2f] dps",
             accel_bias[0], accel_bias[1], accel_bias[2],
             gyro_bias[0], gyro_bias[1], gyro_bias[2]);
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*  采样任务                                                                   */
/* -------------------------------------------------------------------------- */

static void gmeter_task(void *arg)
{
    (void)arg;

    float accel[3] = {0};
    float gyro[3] = {0};
    float lin[3] = {0};

    int64_t last_us = esp_timer_get_time();
    int64_t next_print_us = last_us;
    int64_t next_summary_us = last_us + 1000000;
    int64_t next_beat_us = last_us + 2000000;
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t ok_cnt = 0;
    uint32_t err_cnt = 0;
    bool read_warned = false;
    float gate = 1.0f;  /* 姿态门控系数：1=可信，0=姿态在变，输出全部抑制 */

    while (true) {
        const int64_t now_us = esp_timer_get_time();
        button_poll(now_us);

        if (s_recal_request) {
            s_recal_request = false;
            run_calibration("button");
            last_us = esp_timer_get_time();
        }

        float dt_s = (now_us - last_us) * 1e-6f;
        last_us = now_us;

        if (gmeter_imu_read(accel, gyro) != ESP_OK) {
            err_cnt++;
            if (!read_warned) {
                ESP_LOGW(TAG, "IMU read failed, check I2C (SDA=15, SCL=14) / address");
                read_warned = true;
            }
        } else {
            ok_cnt++;
            gmeter_filter_update(&s_filter, accel, dt_s, lin);

            float pitch = 0.0f, roll = 0.0f;
            gmeter_filter_tilt_deg(&s_filter, &pitch, &roll);

            /*
             * 姿态门控：角速度过大时（手持晃动、快速翻转），重力估计跟不上姿态变化，
             * 扣重力后留下的主要是假信号。此时把送显的线性加速度按 ~100ms 衰减掉。
             */
            const float gyro_mag = sqrtf(gyro[0] * gyro[0] + gyro[1] * gyro[1] +
                                         gyro[2] * gyro[2]);
            const float gate_target = (gyro_mag > GMETER_GYRO_GATE_DPS) ? 0.0f : 1.0f;
            gate += 0.15f * (gate_target - gate);

            const float lin_ui[3] = {lin[0] * gate, lin[1] * gate, lin[2] * gate};

            /* 送给界面：100Hz 写快照，界面按自己的 30fps 读，互不阻塞 */
            gmeter_ui_data_t ui = {
                .lin = {lin_ui[0], lin_ui[1], lin_ui[2]},
                .mag = gmeter_norm3(lin_ui),
                .gyro = {gyro[0], gyro[1], gyro[2]},
                .pitch = pitch,
                .roll = roll,
                .ok = ok_cnt,
                .err = err_cnt,
            };
            gmeter_ui_post(&ui);

            if (now_us >= next_print_us) {
                next_print_us = now_us + GMETER_PRINT_PERIOD_MS * 1000;
                printf("%lld,%.3f,%.3f,%.3f,%.3f,%.1f,%.1f,%.1f\n",
                       (long long)(now_us / 1000),
                       lin[0], lin[1], lin[2], gmeter_norm3(lin),
                       gyro[0], gyro[1], gyro[2]);
                fflush(stdout); /* 明确刷出，避免 stdout 全缓冲导致看不到数据 */
            }

            if (now_us >= next_summary_us) {
                next_summary_us += 1000000;
                printf("# g=[%.2f %.2f %.2f] tilt pitch=%.1f roll=%.1f\n",
                       s_filter.gravity[0], s_filter.gravity[1], s_filter.gravity[2],
                       pitch, roll);
                fflush(stdout);
            }
        }

        /* 心跳：走 ESP_LOG 通道，与 printf 相互印证，判断是任务没跑还是 stdout 没刷 */
        if (now_us >= next_beat_us) {
            next_beat_us = now_us + 2000000;
            ESP_LOGI(TAG, "alive: ok=%lu err=%lu |lin|=%.3f gate=%.2f stack_free=%lu",
                     (unsigned long)ok_cnt, (unsigned long)err_cnt,
                     gmeter_norm3(lin), (double)gate,
                     (unsigned long)uxTaskGetStackHighWaterMark(NULL));
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(GMETER_SAMPLE_PERIOD_MS));
    }
}

/* -------------------------------------------------------------------------- */

void app_main(void)
{
    /* stdout 在 USB-Serial-JTAG 控制台上可能是全缓冲，显式改为行缓冲 */
    setvbuf(stdout, NULL, _IOLBF, 0);

    ESP_LOGI(TAG, "=== ride-gmeter / milestone 1: linear acceleration probe ===");

    esp_err_t err = gmeter_imu_init();
    if (err != ESP_OK) {
        /* 不 abort：保持任务运行，串口里仍能看到心跳与错误计数，便于定位 */
        ESP_LOGE(TAG, "IMU init failed (%s), continuing in degraded mode", esp_err_to_name(err));
    }

    gmeter_filter_init(&s_filter,
                       GMETER_SAMPLE_PERIOD_MS / 1000.0f,
                       GMETER_GRAVITY_TAU_S,
                       GMETER_LP_CUTOFF_HZ,
                       GMETER_DEADBAND_MPS2);

    if (err == ESP_OK && run_calibration("boot") != ESP_OK) {
        ESP_LOGW(TAG, "continuing with zero bias, results may drift");
    }

    /* 显示初始化放在标定之后：屏幕上第一帧就是已标定的结果 */
    esp_err_t ui_err = gmeter_ui_start();
    if (ui_err != ESP_OK) {
        ESP_LOGE(TAG, "UI start failed (%s), running headless", esp_err_to_name(ui_err));
    }

    /* 放在 UI 之后：BSP 的触摸初始化已经装好 GPIO ISR 服务，这里只需挂自己的 handler */
    button_init();

    ESP_LOGI(TAG, "CSV header: t_ms,ax,ay,az,|a|,gx,gy,gz");
    printf("# t_ms,ax,ay,az,|a|,gx,gy,gz\n");
    fflush(stdout);

    BaseType_t ret = xTaskCreatePinnedToCore(gmeter_task, "gmeter",
                                            6144, NULL, 5, NULL, 1);
    ESP_ERROR_CHECK(ret == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
