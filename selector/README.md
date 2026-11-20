# selector

启动选择器，常驻 `factory` 分区（`0x20000`，2MB）。上电后 bootloader 启动它，列出 `storage` 里的镜像，选中后写入运行槽 `ota_0` 并重启。

## 界面

```
   ཱྀ 20:16  ▮100%          ← 状态栏：Wi-Fi | 时间 | 电量
  ┌──────────────────┐
  │ gmeter   1.0.0   │      ← 点一下：安装并进入
  │ xiaozhi  0.1.3   │
  ├──────────────────┤
  │ ⚙ 设置            │      ← 与应用平级，点开设置页
  └──────────────────┘
```

- 背景是 466×466 斜向渐变（`ui_bg.c` 在 PSRAM 里生成一次，三个页面共用），
  角度由 `BG_TILT_DEG` 控制（0° = 竖直，90° = 水平）
- 列表内容超出一屏时可上下拖动
- 安装时覆盖进度浮层；失败时显示原因，点浮层返回列表

## 顶部状态栏

| 元素 | 数据来源 | 显示规则 |
| --- | --- | --- |
| Wi-Fi | `wifi_config_get_state()` | 已连接绿 / 连接中黄 / 未连灰 |
| 时间 | SNTP（`ntp.aliyun.com`，TZ = `CST-8`） | 未同步显示 `--:--`，居中 |
| 电量 | AXP2101（BSP I2C，地址 `0x34`） | 满电绿 / 充电黄（带闪电）/ 其余白，30 s 刷新 |

时间每秒刷新一次；电量低频读取，避免长期占用 I2C。

## 设置页

`⚙ 设置` → 设置页（`settings_ui.c`），页面只创建一次，关闭只是隐藏：

- **Wi-Fi**：副标题显示 SSID / IP / 状态，点进去是扫描 + 密码键盘页（`wifi_setup.c`）
- **扬声器**：0~100 滑杆 + 百分比，拖动时只更新文字，**松手才写 NVS**
  （`components/app_settings`，namespace `app_cfg` / key `spk_vol`，默认 60）
- **存储**：`4.7 / 18.1 MB  26%` + 进度条，分母是 SPIFFS **数据区容量**
  （`esp_spiffs_info()`，已扣掉索引页和 GC 块），不是 21MB 分区容量
- 底部「◀ 返回」退出。圆屏左上角在 y≈30 处可用宽度只有约 220 px，
  44×44 的按钮会被弧线裁掉，所以交互按钮一律放底部安全区

后续加设置项：在 `create_root()` 里追加一行 `make_row(list, LV_SYMBOL_xxx, "标题", cb, &sub)`，
`make_storage_row()` 可参考做纯展示行。

## 触摸手势

LVGL 的 `LV_EVENT_CLICKED` 只判断按下/抬起是否落在同一对象上，手指拖着走几十像素后
松手也算点击；而列表不满一屏时根本没有滚动区域，滑动意图会直接变成「安装 app 并重启」。

所以所有可点对象统一走 `tap_gesture.c`：

- 按下记坐标与 tick，抬起时**位移 ≤ 12 px 且时长 ≤ 800 ms** 才算点击
- 不满足时打印 `selector.tap: drag (dx,dy) ignored, not a tap`，方便调阈值
- 另有一层 800 ms 启动保护（`CLICK_GUARD_MS`），忽略触摸控制器上电时的假触摸

## 模块一览

| 文件 | 职责 |
| --- | --- |
| `selector_ui.c` | 应用列表、顶部状态栏、安装浮层、100 ms 轮询 |
| `settings_ui.c` | 设置页（Wi-Fi 入口 + 存储用量） |
| `wifi_setup.c` | 扫描、密码键盘、保存凭据 |
| `ui_bg.c` | 斜向渐变背景（PSRAM 434 KB，生成一次） |
| `ui_theme.h` | 统一配色（背景、卡片、主色、状态色） |
| `tap_gesture.c` | 点击 / 滑动判定 |
| `battery.c` | AXP2101 电量与充电状态 |
| `clock_sync.c` | SNTP 校时（后台任务） |
| `image_store.c` / `image_install.c` | 镜像库扫描 / 写入 `ota_0` |

## 构建与烧录

```powershell
cd d:\code\esp32_learning\esp32_apps\selector
idf.py set-target esp32s3
idf.py build
..\tools\flash_app.ps1 -Bin build\selector.bin -Slot selector -Port COMx
```

**不要用 `idf.py flash`**：分区表里有 `factory`，IDF 会把 selector 烧到 `0x20000`（这次恰好正确），但同一命令在 app 工程里会把 app 也烧到 `0x20000`，覆盖 selector。统一走 `tools/flash_app.ps1`。

## 工作流程

1. `bsp_display_start()` 初始化屏幕与触摸
2. `battery_init()` 挂上 AXP2101；`clock_sync_start()` 起后台校时任务
3. `image_store_init()` 挂载 `storage`（SPIFFS），失败时格式化一次
4. `image_store_scan()` 枚举 `app-*.bin`，校验 `0xE9` 镜像头与 `esp_app_desc_t` 魔数，跳过无效文件
5. `selector_ui_create()` 建界面；`wifi_config_autoconnect()` 后台连 Wi-Fi
6. 触摸选中 → `image_install_start()`
7. 后台任务校验 `镜像大小 ≤ ota_0 分区大小` → `esp_ota_begin/write/end` → `esp_ota_set_boot_partition(ota_0)` → 重启

## 依赖

- `waveshare/esp32_s3_touch_amoled_1_75c`（BSP，含 LVGL 9.x）
- 共享组件：`../components/boot_switch`、`../components/cjk_font`、`../components/wifi_config`、`../components/app_settings`
- IDF 自带 `spiffs` / `app_update` / `esp_app_format` / `esp_wifi` / `esp_netif` / `esp_driver_i2c`

## 已知限制

- 时区硬编码 `CST-8`；时间只存在 RAM，每次进 selector 都要重新校时（没写 NVS）
- 电量百分比优先取 AXP2101 的 `0xA4`，读不到才按电压曲线（3.30 V→0%，4.20 V→100%）估算
- 暂无自动启动上次 app 的倒计时（M4）
- 只显示 `esp_app_desc.project_name` 与 `version`，还没用 `manifest.json` 里的中文名和图标
- 没有删除 / 卸载镜像的入口
- 镜像大小超过 `ota_0`（8MB）时直接报错，需要改分区表
