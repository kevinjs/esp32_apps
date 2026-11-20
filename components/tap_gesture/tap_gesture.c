#include "tap_gesture.h"

#include <stdint.h>

#include "esp_log.h"

static const char *TAG = "tap";

#define TAP_MAX_DIST_PX 12
#define TAP_MAX_MS      800

static struct {
    lv_point_t start;
    uint32_t   tick;
    bool       valid;
} s_press;

static void press_handler(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_PRESS_LOST) {
        s_press.valid = false;
        return;
    }

    lv_indev_t *indev = lv_indev_active();
    s_press.valid = false;
    if (indev == NULL) {
        return;
    }

    lv_indev_get_point(indev, &s_press.start);
    s_press.tick = lv_tick_get();
    s_press.valid = true;
}

void tap_gesture_attach(lv_obj_t *obj)
{
    if (obj == NULL) {
        return;
    }
    lv_obj_add_event_cb(obj, press_handler, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(obj, press_handler, LV_EVENT_PRESS_LOST, NULL);
}

bool tap_gesture_was_tap(void)
{
    if (!s_press.valid) {
        return false;
    }
    s_press.valid = false;

    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return false;
    }

    lv_point_t now;
    lv_indev_get_point(indev, &now);
    const int32_t dx = now.x - s_press.start.x;
    const int32_t dy = now.y - s_press.start.y;

    if ((dx * dx + dy * dy) > (TAP_MAX_DIST_PX * TAP_MAX_DIST_PX)) {
        ESP_LOGD(TAG, "drag (%d,%d) ignored", (int)dx, (int)dy);
        return false;
    }
    if (lv_tick_elaps(s_press.tick) > TAP_MAX_MS) {
        ESP_LOGD(TAG, "long press ignored");
        return false;
    }
    return true;
}
