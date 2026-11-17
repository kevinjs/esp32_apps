/*
 * 启动切换 / 回退组件
 *
 * 每个应用都必须链接本组件，否则进入应用后无法回到 factory 里的启动选择器
 * （bootloader 直接跳 otadata 指向的槽，不会经过 factory）。
 *
 * 方案说明见 docs/multi-app-image-management.md
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOOT_SWITCH_SELECTOR_LABEL      "factory"
#define BOOT_SWITCH_RUN_SLOT_LABEL      "ota_0"

#define BOOT_SWITCH_MAX_ATTEMPTS_DEFAULT    3
#define BOOT_SWITCH_KEY_GPIO_DEFAULT        0       /* BOOT 键 = GPIO0 */
#define BOOT_SWITCH_KEY_HOLD_MS_DEFAULT     3000

typedef struct {
    /** 连续多少次启动未 mark_ok 就自动回 selector，0 表示关闭 */
    uint8_t max_boot_attempts;
    /** 是否启动后台任务，长按 BOOT 键强制回 selector */
    bool watch_boot_key;
    int boot_key_gpio;
    uint32_t boot_key_hold_ms;
} boot_switch_config_t;

#define BOOT_SWITCH_CONFIG_DEFAULT() {          \
    .max_boot_attempts = BOOT_SWITCH_MAX_ATTEMPTS_DEFAULT, \
    .watch_boot_key = true,                     \
    .boot_key_gpio = BOOT_SWITCH_KEY_GPIO_DEFAULT,         \
    .boot_key_hold_ms = BOOT_SWITCH_KEY_HOLD_MS_DEFAULT,   \
}

/**
 * @brief 初始化。必须尽早调用（app_main 开头，NVS 初始化之后）。
 *
 * - 运行在 selector(factory) 里时不做启动计数；
 * - 运行在 app 槽里时累加启动次数，超过 max_boot_attempts 直接回 selector；
 * - config 传 NULL 使用 BOOT_SWITCH_CONFIG_DEFAULT()。
 */
esp_err_t boot_switch_init(const boot_switch_config_t *config);

/**
 * @brief 标记本次启动成功，清零失败计数。建议在 UI 首帧出来之后调用。
 */
esp_err_t boot_switch_mark_boot_ok(void);

/**
 * @brief 切回 selector 并立即重启（擦除 otadata，bootloader 回落到 factory）。
 */
esp_err_t boot_switch_request_selector(void);

/**
 * @brief 设置下次启动的 app 分区，不重启。成功返回后需要自行 esp_restart()。
 *
 * @param label 分区标签，如 "ota_0"
 */
esp_err_t boot_switch_set_boot_slot(const char *label);

/** @brief 当前运行分区的标签，未知时返回 "unknown"。 */
const char *boot_switch_running_slot(void);

/** @brief 当前是否运行在 selector(factory) 分区。 */
bool boot_switch_running_from_selector(void);

#ifdef __cplusplus
}
#endif
