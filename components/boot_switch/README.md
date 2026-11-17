# boot_switch

每个应用都必须链接的启动切换组件。

## 为什么需要它

设备分区表是 `factory(selector) + ota_0(运行槽) + storage(镜像库)`。bootloader 按 `otadata` 直接跳到 `ota_0`，**不会经过 factory**。所以"回到启动选择器"的能力必须在应用内部，否则一旦进入 app，没有 PC 就出不来。

## 接入方式

工程根目录的 `CMakeLists.txt` 增加共享组件目录：

```cmake
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")
```

`main/CMakeLists.txt` 增加依赖：

```cmake
idf_component_register(SRCS "main.c" INCLUDE_DIRS "." REQUIRES boot_switch)
```

`app_main()` 里：

```c
#include "boot_switch.h"

void app_main(void)
{
    /* NVS 由应用自行初始化；组件只打开自己的命名空间 */
    ESP_ERROR_CHECK(nvs_flash_init());

    boot_switch_config_t cfg = BOOT_SWITCH_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(boot_switch_init(&cfg));

    /* ... 应用初始化 ... */

    /* UI 首帧出来之后再标记成功，避免把"启动即崩"算成成功 */
    boot_switch_mark_boot_ok();
}
```

## 行为

| 场景 | 行为 |
| --- | --- |
| 运行在 `factory`(selector) | 不做启动计数，不启用按键监听 |
| 运行在 `ota_0`，正常启动 | NVS `tries` 自增，`mark_boot_ok()` 后清零 |
| 连续 `max_boot_attempts` 次未标记成功 | 擦除 `otadata`，重启回 selector |
| 长按 BOOT 键 `boot_key_hold_ms` | 擦除 `otadata`，重启回 selector |

## 注意

- **不要在上电时按住 BOOT 想"进选择器"**：GPIO0 是 download strap，会直接进 ROM 下载模式。只支持运行时长按。
- 回 selector 的实现是擦除 `otadata`，依赖 bootloader 的"otadata 无效则启动 factory"行为，不依赖 `esp_ota_set_boot_partition(factory)` 的子类型特殊处理。
- 组件不会擦除 NVS。若 NVS 版本不匹配，会记录错误并跳过计数，避免把 IMU 标定数据一起清掉。
