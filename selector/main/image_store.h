/*
 * 镜像库扫描：挂载 storage 分区（SPIFFS），枚举 app-*.bin 并解析 app 描述符。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IMAGE_STORE_MAX     16
#define IMAGE_FILE_LEN      40      /* SPIFFS 对象名上限 31，留余量 */
#define IMAGE_LABEL_LEN     33      /* esp_app_desc 里 version/project_name 是 char[32] */

typedef struct {
    char file[IMAGE_FILE_LEN];          /* 文件名，如 app-gmeter-1.0.0.bin */
    char name[IMAGE_LABEL_LEN];         /* esp_app_desc.project_name */
    char version[IMAGE_LABEL_LEN];      /* esp_app_desc.version */
    uint32_t size;                      /* 文件字节数 */
} image_entry_t;

/**
 * @brief 挂载 storage。挂载失败时会尝试格式化一次（此时镜像库为空）。
 */
esp_err_t image_store_init(void);

/**
 * @brief 扫描镜像库，按文件名排序。无效镜像（非 0xE9 头 / 描述符魔数不对）会被跳过。
 *
 * @return 实际条目数（0~max_count）
 */
int image_store_scan(image_entry_t *out, int max_count);

/** @brief 查询 SPIFFS 容量，失败时返回 0。 */
void image_store_info(size_t *total_bytes, size_t *used_bytes);

/**
 * @brief 读 app 的图标（pack_storage 转好的 LVGL 二进制：12 字节 header + 像素）
 *
 * 文件是 `icon-<id>.bin`（由 `icon-<id>.png` 打包时转换而来）；数据从 PSRAM 分配，
 * 调用方用完自行 heap_caps_free()。
 *
 * @param app_file app 镜像文件名，如 `app-watch-0.1.5.bin`
 * @param[out] out     图标数据（含 lv_image_header_t）
 * @param[out] out_size 字节数
 * @return true = 有这个图标且读取成功
 */
bool image_store_read_icon(const char *app_file, uint8_t **out, size_t *out_size);

#ifdef __cplusplus
}
#endif
