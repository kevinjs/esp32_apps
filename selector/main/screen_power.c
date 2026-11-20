/*
 * 息屏 / 亮屏
 *
 * 息屏 = 背光 0% + 禁用 LVGL 触摸设备，界面照常在后台刷新但不再响应点击；
 * 亮屏 = 恢复进入时光标亮度并重新启用触摸。
 * （AMOLED 黑屏本身不耗电，这里不需要关 panel，唤醒也就没有初始化延迟。）
 */
#include "screen_power.h"

#include "esp_log.h"
#include "lvgl.h"

#include "bsp/esp-bsp.h"

static const char *TAG = "selector.screen";

static bool s_off = false;
static int s_saved_brightness = 100;
static lv_indev_t *s_touch = NULL;

void screen_power_init(void)
{
    s_touch = bsp_display_get_input_dev();

    s_saved_brightness = bsp_display_brightness_get();
    if (s_saved_brightness <= 0) {
        s_saved_brightness = 100;
    }

    ESP_LOGI(TAG, "ready: brightness %d%%, touch %s",
             s_saved_brightness, s_touch != NULL ? "ok" : "missing");
}

bool screen_power_is_off(void)
{
    return s_off;
}

void screen_power_toggle(void)
{
    s_off = !s_off;

    if (s_off) {
        bsp_display_brightness_set(0);
        if (s_touch != NULL) {
            lv_indev_enable(s_touch, false);
        }
        ESP_LOGI(TAG, "display off, touch disabled");
    } else {
        bsp_display_brightness_set(s_saved_brightness);
        if (s_touch != NULL) {
            lv_indev_enable(s_touch, true);
        }
        ESP_LOGI(TAG, "display on, touch enabled");
    }
}
