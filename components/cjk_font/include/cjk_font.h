/*
 * 全局中文字库（供 selector 与所有 app 共用）
 *
 * 字库不编进任何 app 的镜像，而是放在 storage 分区（SPIFFS）里的
 * font-cjk.ttf（GB2312 子集，约 2 MB），由本组件在启动时读进 PSRAM，
 * 再用 LVGL tiny_ttf 按需渲染。
 *
 * 这样做是因为 selector 的 factory 分区只有 2 MB，塞不下字库；
 * 而每个 app 各带一份 2 MB 也不划算。
 *
 * 用法：
 *     cjk_font_init();                      // app_main 里，bsp_display_start 之后
 *     lv_obj_set_style_text_font(label, cjk_font_or(16, &lv_font_montserrat_16), 0);
 *
 * 字库缺失/损坏时 cjk_font() 返回 NULL，调用方用 cjk_font_or() 自动回退，
 * 界面照常显示（中文会缺字形），不会崩。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 字库文件在 storage 分区里的文件名（SPIFFS 对象名上限 31 字符）。 */
#define CJK_FONT_FILE   "font-cjk.ttf"

/**
 * @brief 挂载 storage（若尚未挂载）并把字库读进 PSRAM。
 *
 * 必须在 lv_init() 之后调用（内部会创建 tiny_ttf 字体）。可重复调用。
 *
 * @return ESP_OK 成功；ESP_ERR_NOT_FOUND 表示镜像库里没有字库文件。
 */
esp_err_t cjk_font_init(void);

/**
 * @brief 取指定像素字号的中文（+ASCII）字体，首次调用时创建并缓存。
 *
 * @param px 字号，建议 12~32
 * @return 字体对象；字库未加载或创建失败返回 NULL
 */
const lv_font_t *cjk_font(int px);

/**
 * @brief 取中文字体，不可用时返回 fallback。
 */
const lv_font_t *cjk_font_or(int px, const lv_font_t *fallback);

/** 字库是否已成功加载。 */
bool cjk_font_ready(void);

#ifdef __cplusplus
}
#endif
