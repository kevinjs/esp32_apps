# watch · 多功能怀表

466×466 圆形 AMOLED 上的「表盘 + 秒表」。离线可用，联网后自动校时。

| 项 | 内容 |
| --- | --- |
| 硬件 | Waveshare ESP32-S3-Touch-AMOLED-1.75C（ESP32-S3R8 / 8MB PSRAM / 32MB Flash / 466×466 AMOLED） |
| 框架 | ESP-IDF v6.1（v5.5+ 亦可），LVGL 9.6，组件 `waveshare/esp32_s3_touch_amoled_1_75c` |
| 共享组件 | `boot_switch`（长按 BOOT 回 selector）、`cjk_font`（storage 里的共享中文字库）、`wifi_config`（只读凭证）、`tap_gesture`（防误触） |
| 状态 | 可用：表盘 / 秒表 / 触摸与 BOOT 双操作 |

## 构建与发布

```powershell
cd d:\code\esp32_learning\esp32_apps\watch
idf.py build
# 产物：build\watch.bin（约 1.4 MB，ota_0 槽 8MB 完全够用）
```

**禁止 `idf.py flash`**：它固定写 `0x20000`（factory = selector），会覆盖启动选择器。
正确发布流程见 [`../docs/multi-app-image-management.md`](../docs/multi-app-image-management.md)：

```powershell
copy build\watch.bin ..\storage_src\app-watch-0.1.0.bin   # 一个 id 只能留一份 .bin
..\tools\pack_storage.ps1 -Flash                           # 重写 21MB 镜像库
# 再从 selector 里选 watch 启动；需要恢复 selector 时用 tools\flash_app.ps1 -Bin xx.bin -Slot selector
```

## 交互

| 操作 | 效果 |
| --- | --- |
| 左右滑动 | 表盘 ⇄ 秒表（松手自动吸附到整页） |
| BOOT 短按（< 600 ms） | 开始 / 暂停，并自动切到秒表页 |
| BOOT 按住 600 ~ 2900 ms 后松开 | 记一段（仅运行中有效） |
| BOOT 按住 3000 ms | 由 `boot_switch` 接管，重启回 selector |
| 屏幕「开始/继续/暂停」 | 同上，按钮文字与颜色随状态变化 |
| 屏幕「记圈」 | 记一段；满 5 段后保留最近 5 段 |
| 屏幕「复位」 | 清零；**运行中会被忽略**（先暂停再复位，避免误抹成绩） |

复位只在屏幕上做，键位不冲突；BOOT 键从 selector 里借了 3 秒长按，所以"记圈"窗口上限取 2900 ms。

## 界面

**表盘页**（模拟指针）：整屏铺 selector 同款 30° 斜向渐变；12 条放射状刻度
（3/6/9/12 点更长更粗，白）+ 整点数字 1~12（12/3/6/9 为 20 px 白，其余 16 px 灰）
+ 时针/分针/秒针 + 中心轴帽。

日期与星期做成机械表式「日期窗」，夹在圆心与 3 点钟数字之间（`x=+108`，84×42，青色细边）：
第一行「11月20日」、第二行「周五」。不显示数字时间，也不显示「已校时 · 在线」之类的状态——
时间不可信时日期窗变成琥珀色「等待校时」，同时隐藏指针
（摆在 12 点会让人误读成零点）。

底部两个页码点放在整点数字（r=166）与 6 点钟长刻度之间（`y=192`，左右各偏 18 px），互不粘连。

**秒表页**：`MM:SS.CC`（超过 1 小时自动变 `H:MM:SS`）、状态（就绪 / 计时中 / 已暂停）、
5 行圈速（左：本段用时，右：累计时间）、底部三个圆按钮。

## 时间源

板上没有 RTC，只有两条时间来源：

1. **NVS 恢复**：每次运行中每 5 分钟把 UTC 写回 NVS（namespace `watch`，key `utc`），
   开机先把系统时间设回去，表盘立刻是"上次的时间"。
2. **SNTP 校时**：Wi-Fi 连上后向 `ntp.aliyun.com` 同步，成功后写回 NVS。

时区固定 `CST-8`（`watch_time.c` 里的 `CLOCK_TZ`）。Wi-Fi 凭证由 selector 的设置页写入 NVS，
本 app 只读不发；没配网时表盘仍可用，只是每次开机会慢一点（关机时长未计入）。

## 实现要点

| 主题 | 做法 | 原因 |
| --- | --- | --- |
| 计时基准 | `esp_timer_get_time()`（微秒，单调） | SNTP 校时会让系统时间跳变，用 `time()` 计时会被"改表"破坏 |
| 并发 | `portMUX` 临界区保护状态，UI 只读快照 `watch_stopwatch_snapshot_t` | BOOT 键任务与 LVGL 定时器（两个上下文）并发读写同一份状态 |
| 界面解耦 | 界面不保存秒表状态，四个 API（toggle/reset/lap/snapshot）收敛全部操作 | 重建界面不会丢数据，键和按钮天然共用一套逻辑 |
| 指针绘制 | 三根指针各是一个 `lv_line`，每次秒变化重算两个端点（`0° = 12 点`，顺时针） | 纯矢量、无需旋转图片；点数组必须是静态的（`lv_line` 只保存指针） |
| 指针不压刻度 | 时针 90 / 分针 126 / 秒针 148 px，刻度内端 182 / 192，数字在 r=166 | 针尖到数字圈之外才停，读数不被遮挡 |
| 日期窗位置 | 中心与 3 点钟数字之间（`x=+108`），84×42 圆角窗 | 机械表常见布局；指针扫过时压在窗上，观感与真表一致 |
| 翻页不漂移 | `watch_ui_show_page()` 已在目标页就直接返回；`SCROLL_END` 里把 `scroll_x` 对齐到整像素 | BOOT 键每按一次都会调 `show_stopwatch()`，重复触发滚动动画会被 snap 拖偏几像素 |
| 背景 | 与 selector 共用 `components/ui_bg`：PSRAM 里生成一张 30° 斜向渐变图当 `bg_image` | 一处实现两处使用，角度与配色不会各自走样 |
| 圈速容量 | 满 5 段后 `memmove` 左移，保留最近 5 段 | 固定内存，无动态分配 |
| 翻页 | 两页并排放在只能横向滚动、`SCROLL_SNAP_CENTER` 的容器里 | 不用自己做手势识别，松手自动对齐 |
| 防误触 | 按钮走 `tap_gesture`：按下后拖动超过 12px 或超过 800ms 松手不算点击 | 在按钮上滑动翻页时不会误触发 |
| 刷新 | 单个 100ms LVGL 定时器 + 文本变化才 set | 百分秒只需 10Hz，省刷新省电 |

## 代码结构

| 文件 | 职责 |
| --- | --- |
| `main/main.c` | 启动顺序、BOOT 键轮询与分发 |
| `main/watch_ui.c` / `.h` | 表盘页与秒表页、翻页、100ms 刷新 |
| `main/watch_time.c` / `.h` | NVS 恢复 + SNTP 校时，`watch_time_get()` 给界面用 |
| `main/watch_stopwatch.c` / `.h` | 与界面无关的秒表状态机（线程安全） |
| `main/watch_config.h` | 按键时间窗、刷新周期、界面配色 |
| `../components/ui_bg` | 斜向渐变背景（selector 与 watch 共用同一份实现） |

## 可调参数

全部在 `main/watch_config.h`：

| 宏 | 默认 | 说明 |
| --- | --- | --- |
| `WATCH_BOOT_TAP_MAX_MS` | 600 | 短按上限（开始/暂停） |
| `WATCH_BOOT_LAP_MAX_MS` | 2900 | 记圈窗口上限，必须小于 `WATCH_BOOT_SELECTOR_MS` |
| `WATCH_BOOT_SELECTOR_MS` | 3000 | 必须与 `boot_switch` 的 `boot_key_hold_ms` 一致 |
| `WATCH_TICK_MS` | 100 | 界面刷新周期 |

## 已知限制

1. **进度不跨重启**：秒表状态只在 RAM 里，回 selector 或断电即清零（成绩想留存需要另做 NVS 落盘）。
2. **关机期间时间不流逝的补偿很粗**：只按"上次同步的 UTC"恢复，不累计关机时长，
   所以离线开机的绝对时间会落后；联网后 SNTP 一次性纠正。
3. 圈速固定 5 行，超出的老段被移出显示（数据本身只保留最近 5 段）。
4. 没有息屏/低功耗策略，AMOLED 常亮；续航优化（亮度、降刷新、PWR 键息屏）留待后续。
