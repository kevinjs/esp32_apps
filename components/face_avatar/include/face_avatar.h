/*
 * 设备统一的拟人形象（表情包）：公共资源，放在 storage 分区，
 * 由 selector 与所有 app 共用一份，不编进任何镜像。
 *
 * 素材来源：storage_src/faces/face-<name>.png
 * 打包产物：storage:/face-<name>.bin（12 字节 lv_image_header + ARGB8888 像素）
 *
 * 固件不做 PNG 解码：tools/pack_storage.ps1 在 PC 上把 PNG 缩放、裁剪、对齐后
 * 转成 LVGL 原生二进制，这里只负责整块读进 PSRAM 当 lv_image_dsc_t 用。
 *
 * 换表情只需要重新打包 storage，不用重编任何 app。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 表情种类：与 storage 里的 face-<name>.bin 一一对应。 */
typedef enum {
    FACE_AVATAR_WAIT = 0,   /* 启动 / 联网 / 连接中 */
    FACE_AVATAR_IDLE,       /* 待命 */
    FACE_AVATAR_BLINK,      /* 待命时眨眼的中间帧 */
    FACE_AVATAR_LISTEN,     /* 聆听 */
    FACE_AVATAR_THINK,      /* 识别中 / 思考 */
    FACE_AVATAR_SAD,        /* 出错 / 断开 */
    FACE_AVATAR_HAPPY,      /* 一轮完成（可选素材） */
    FACE_AVATAR_TALK,       /* 播报：多帧，用 face_avatar_talk_frame() 取 */
    FACE_AVATAR_MOOD_NUM,
} face_avatar_mood_t;

/**
 * @brief 挂载 storage 并预载全部表情到 PSRAM。
 *
 * 幂等：重复调用直接返回。素材缺失时返回 ESP_ERR_NOT_FOUND，
 * 调用方应回退到自绘的几何表情。
 */
esp_err_t face_avatar_init(void);

/** 是否已成功加载（至少 idle 可用）。 */
bool face_avatar_ready(void);

/** 表情边长（像素），未加载时为 0。用于布局。 */
uint16_t face_avatar_size(void);

/** 取某个表情；首次访问时从 storage 读入 PSRAM，失败返回 NULL。 */
const lv_image_dsc_t *face_avatar_get(face_avatar_mood_t mood);

/** 按名字取（不含 "face-" 前缀与 ".bin" 后缀），供特殊需求使用。 */
const lv_image_dsc_t *face_avatar_by_name(const char *name);

/** 播报动画帧数（face-talk-1..N.bin），0 表示素材没带。 */
int face_avatar_talk_frames(void);

/** 第 index 帧播报画面；越界返回 NULL。 */
const lv_image_dsc_t *face_avatar_talk_frame(int index);

/** 释放全部 PSRAM（一般不用调，切换 app 会重启）。 */
void face_avatar_release(void);

#ifdef __cplusplus
}
#endif
