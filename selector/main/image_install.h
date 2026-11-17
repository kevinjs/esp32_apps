/*
 * 把 storage 里的 app 镜像写入运行槽 ota_0，并切换启动分区。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IMAGE_INSTALL_IDLE = 0,
    IMAGE_INSTALL_RUNNING,
    IMAGE_INSTALL_DONE,
    IMAGE_INSTALL_FAILED,
} image_install_state_t;

typedef struct {
    image_install_state_t state;
    int percent;                /* 0~100 */
    char message[64];           /* 失败原因 / 当前阶段 */
} image_install_status_t;

/**
 * @brief 启动后台安装任务。同一时间只允许一个任务。
 *
 * @param file storage 里的文件名，如 "app-gmeter-1.0.0.bin"
 */
esp_err_t image_install_start(const char *file);

/** @brief 读取当前状态（线程安全）。 */
void image_install_status(image_install_status_t *out);

/** @brief 失败后复位回 IDLE，允许重新选择。 */
void image_install_reset(void);

#ifdef __cplusplus
}
#endif
