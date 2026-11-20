/*
 * AXP2101 电池读数
 *
 * 寄存器（与 XPowersLib / 官方示例一致）：
 *   0x00 STATUS1      bit3 = 电池在位
 *   0x01 STATUS2  [7:5] = 充放电状态，1 = 正在充电
 *   0x30 ADC_CH_CTRL  bit0 = 电池电压 ADC 使能
 *   0x34/0x35        电池电压，13 bit，单位 mV（高 5 位 + 低 8 位）
 *   0xA4             电量百分比 0..100
 */
#include "battery.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"

#include "bsp/esp-bsp.h"

static const char *TAG = "selector.bat";

#define AXP2101_ADDR         0x34
#define AXP_REG_STATUS1      0x00
#define AXP_REG_STATUS2      0x01
#define AXP_REG_ADC_CH_CTRL  0x30
#define AXP_REG_BAT_VOLT_H   0x34
#define AXP_REG_BAT_VOLT_L   0x35
#define AXP_REG_INTEN2       0x41
#define AXP_REG_INTSTS2      0x49
#define AXP_REG_BAT_PERCENT  0xA4

/* PWRKEY 短按是 24 bit 中断里的 bit11，落在 INTEN2/INTSTS2 的 bit3 */
#define AXP_PKEY_SHORT_BIT   0x08

#define I2C_TIMEOUT_MS       100

static i2c_master_dev_handle_t s_dev = NULL;

static esp_err_t reg_read(uint8_t reg, uint8_t *value)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(s_dev, &reg, 1, value, 1, I2C_TIMEOUT_MS);
}

static esp_err_t reg_update_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t current = 0;
    esp_err_t err = reg_read(reg, &current);
    if (err != ESP_OK) {
        return err;
    }
    const uint8_t next = (uint8_t)((current & ~mask) | (value & mask));
    if (next == current) {
        return ESP_OK;
    }
    const uint8_t buf[2] = { reg, next };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

esp_err_t battery_init(void)
{
    if (s_dev != NULL) {
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        ESP_LOGW(TAG, "I2C bus unavailable, battery stays unknown");
        return ESP_ERR_INVALID_STATE;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = 100000,
    };
    const esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 not found: %s", esp_err_to_name(err));
        s_dev = NULL;
        return err;
    }

    /* 电池电压 ADC 可能没使能，打开一次（幂等，不影响其他通道） */
    reg_update_bits(AXP_REG_ADC_CH_CTRL, 0x01, 0x01);
    ESP_LOGI(TAG, "AXP2101 ready");
    return ESP_OK;
}

static uint16_t read_millivolt(void)
{
    uint8_t hi = 0;
    uint8_t lo = 0;
    if (reg_read(AXP_REG_BAT_VOLT_H, &hi) != ESP_OK ||
        reg_read(AXP_REG_BAT_VOLT_L, &lo) != ESP_OK) {
        return 0;
    }
    return (uint16_t)(((hi & 0x1F) << 8) | lo);
}

/* 没有电量计时按锂电电压粗算：3.30 V → 0%，4.20 V → 100% */
static uint8_t percent_from_millivolt(uint16_t mv)
{
    if (mv <= 3300) {
        return 0;
    }
    if (mv >= 4200) {
        return 100;
    }
    return (uint8_t)(((mv - 3300) * 100) / 900);
}

esp_err_t battery_read(battery_info_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    uint8_t status1 = 0;
    uint8_t status2 = 0;
    uint8_t percent = 0;

    if (reg_read(AXP_REG_STATUS1, &status1) != ESP_OK) {
        return ESP_FAIL;
    }
    out->present = (status1 & 0x08) != 0;

    if (reg_read(AXP_REG_STATUS2, &status2) == ESP_OK) {
        out->charging = ((status2 >> 5) & 0x07) == 0x01;
    }

    if (!out->present) {
        return ESP_OK;
    }

    out->millivolt = read_millivolt();
    if (reg_read(AXP_REG_BAT_PERCENT, &percent) == ESP_OK && percent <= 100) {
        out->percent = percent;
    } else {
        out->percent = percent_from_millivolt(out->millivolt);
    }
    return ESP_OK;
}

/* 板上没有把 AXP2101 的 IRQ 引脚接到 ESP32，只能读 INTSTS 轮询；写 1 清除 */
static void clear_pkey_short(void)
{
    const uint8_t buf[2] = { AXP_REG_INTSTS2, AXP_PKEY_SHORT_BIT };
    i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

esp_err_t battery_power_key_init(void)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t status = 0;
    if (reg_read(AXP_REG_INTSTS2, &status) == ESP_OK && (status & AXP_PKEY_SHORT_BIT) != 0) {
        clear_pkey_short();   /* 上电过程中可能残留一次，先清掉 */
    }

    const esp_err_t err = reg_update_bits(AXP_REG_INTEN2, AXP_PKEY_SHORT_BIT, AXP_PKEY_SHORT_BIT);
    ESP_LOGI(TAG, "PWRKEY short-press irq %s", err == ESP_OK ? "enabled" : "failed");
    return err;
}

bool battery_power_key_clicked(void)
{
    if (s_dev == NULL) {
        return false;
    }

    uint8_t status = 0;
    if (reg_read(AXP_REG_INTSTS2, &status) != ESP_OK) {
        return false;
    }
    if ((status & AXP_PKEY_SHORT_BIT) == 0) {
        return false;
    }

    clear_pkey_short();
    return true;
}
