# selector

启动选择器，常驻 `factory` 分区（`0x20000`，2MB）。上电后 bootloader 启动它，列出 `storage` 里的镜像，选中后写入运行槽 `ota_0` 并重启。

## 界面

```
   ཱྀ 20:16  ▮100%          ← 状态栏：Wi-Fi | 时间 | 电量
   ┌────────┐ ┌────────┐
   │  ⌚ 表  │ │  🏍 骑  │    ← 每格 150×150 圆角磁贴，图案"烙印"在纯色底上
   │  怀表  │ │  骑行  │
   └────────┘ └────────┘
   ┌────────┐ ┌────────┐
   │  🗣 智  │ │  ⚙ 设置 │    ← 设置固定占最后一格
   │  小智  │ │Wi-Fi/存储│
   └────────┘ └────────┘
        ·  ●                 ← 页码点，左右滑动翻页
```

- 每屏 2×2 共 4 个磁贴（132×132，间距 20；整块 284×284，四角到圆心 201 < 233）
- 磁贴 = 纯色圆角矩形 + 图标：
  - **有 `icon-<id>.png`** → 用它（`pack_storage.ps1` 转成 88×88 LVGL 二进制，运行时读进 PSRAM）
  - 没图标时回退「烙印」：已知 app 用固定汉字（`APP_LOOKS`：`watch`/`gmeter`/`xiaozhi`），
    未知 app 取项目名首字母 + 文件名哈希配色，**新增 app 不用改代码**
  - 烙印用两层叠出凹凸感：比底色亮 55%（高光）+ 比底色暗 50%（刻痕）
- 超过 4 个 app 自动排到后面的页，左右滑动切换（滚动容器 + `SCROLL_SNAP_CENTER` 吸附），底部页码点
- 翻页过程中磁贴按离屏幕中心的距离缩放（256→243，即 100%→95%）并淡出（255→199），停下回 100%
- 背景是 466×466 斜向渐变（`components/ui_bg` 在 PSRAM 里生成一次，所有页面共用），
  角度由 `UI_BG_TILT_DEG` 控制（0° = 竖直，90° = 水平）
- 安装时覆盖进度浮层；失败时显示原因，点浮层返回

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
- **亮度**：10~100 滑杆（下限 10，调到 0 会黑屏摸不到），交互与音量一致；
  拖动时实时 `bsp_display_brightness_set()` 预览，松手写 NVS（key `bright`，默认 80）。
  app 启动时自己读一次这个值套到背光上，所以进任何 app 亮度都不跳变
- **存储**：`4.7 / 18.1 MB  26%` + 进度条，分母是 SPIFFS **数据区容量**
  （`esp_spiffs_info()`，已扣掉索引页和 GC 块），不是 21MB 分区容量；
  点进去是存储详情页（见下）
- 底部「◀ 返回」退出。圆屏左上角在 y≈30 处可用宽度只有约 220 px，
  44×44 的按钮会被弧线裁掉，所以交互按钮一律放底部安全区

后续加设置项：在 `create_root()` 里追加一行 `make_row(list, LV_SYMBOL_xxx, "标题", cb, &sub)`。

## 图标

图标不放在固件里，而是跟着镜像库走：

```
storage_src/icon-<id>.png          256×256 白色线条 + 透明底（源图）
        ↓ pack_storage.ps1 用 System.Drawing 缩放居中、转 ARGB8888
storage 分区 icon-<id>.bin         12 字节 lv_image_header + 88×88 像素（约 31 KB）
        ↓ 运行时 image_store_read_icon() 读进 PSRAM
selector 磁贴 lv_image              直接当 lv_image_dsc_t 用，不经过任何解码器
```

**为什么不在固件里解 PNG**：开 libpng/lodepng 要上百 KB flash，解码缓冲还会落在
LVGL 那个只有几十 KB 的内部堆上（88×88 ARGB8888 一次就要 31 KB），风险高。
在打包机上转成 LVGL 原生格式，运行时只是 `fread` + 结构体赋值。

换图标：把新的 `icon-<id>.png` 放进 `storage_src`，重跑 `pack_storage.ps1 -Flash`，
不用重新编译 selector。没有图标的 app 自动回退到烙印汉字/首字母。

## 存储详情页

设置 →「存储」进入（`storage_ui.c`），每次打开都重新扫描，`pack_storage.ps1` 之后不用重启：

- 顶部汇总卡：`已用 4.7 / 18.1 MB  26%` + 进度条 + `剩余 13.4 MB · 3 个应用`
- 列表逐条列出镜像库里的 app：名称、版本、文件名、占用空间（< 1 MB 显示 KB）
- 末尾一行「共享资源」= 已用总量 − 各 app 之和，用来解释字库等非 app 文件
  占了空间（不然用户会以为容量算错了）
- 行数超过可视区域时可上下滚；底部「◀ 返回」

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
| `selector_ui.c` | 应用磁贴网格、顶部状态栏、安装浮层、100 ms 轮询 |
| `settings_ui.c` | 设置页（Wi-Fi 入口 + 扬声器 + 存储入口） |
| `storage_ui.c` | 存储详情页（用量汇总 + 各 app 占用） |
| `wifi_setup.c` | 扫描、密码键盘、保存凭据 |
| `components/ui_bg` | 斜向渐变背景（PSRAM 434 KB，生成一次，各 app 共用） |
| `ui_theme.h` | 统一配色（背景、卡片、主色、状态色） |
| `components/tap_gesture` | 点击 / 滑动判定（各 app 共用） |
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
