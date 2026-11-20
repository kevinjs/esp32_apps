/*
 * 电池状态（AXP2101 PMU，走 BSP 的 I2C 总线）
 *
 * 板上没有独立的电量计芯片，电压/百分比都从 AXP2101 的 ADC 寄存器读。
 * 读失败不影响 UI，调用方按 present=false 处理即可。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool     present;    /* 是否检测到电池（插着 USB 没电池时为 false） */
    bool     charging;   /* 正在充电 */
    uint8_t  percent;    /* 0..100，读不到时为 0 */
    uint16_t millivolt;  /* 电池电压 mV，0 = 读不到 */
} battery_info_t;

/** @brief 挂到 BSP 的 I2C 总线上，幂等；失败后 battery_read() 直接返回错误 */
esp_err_t battery_init(void);

/** @brief 读一次电池状态；无电池时返回 ESP_OK 且 present=false */
esp_err_t battery_read(battery_info_t *out);

/**
 * @brief 使能电源键（AXP2101 PWRKEY）短按中断位
 *
 * 板上 IRQ 引脚没接到 ESP32，所以用轮询 INTSTS 的方式，见 battery_power_key_clicked()。
 * 需要先 battery_init() 成功。
 */
esp_err_t battery_power_key_init(void);

/** @brief 轮询一次：检测到电源键短按返回 true，并清除状态位 */
bool battery_power_key_clicked(void);
