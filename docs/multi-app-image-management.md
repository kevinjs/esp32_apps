# 多 app 镜像管理方案（讨论记录与决策）

- 日期：2026-11-17
- 目标硬件：Waveshare ESP32-S3-Touch-AMOLED-1.75C（ESP32-S3R8 / 8MB PSRAM / 32MB Flash / 466×466 AMOLED / QMI8658 / WiFi）
- 状态：决策已定，开始落地

## 1. 背景

这块板子要作为"多场景设备"使用：小智 AI 对话、速度计（ride-gmeter）、音乐节拍指示器、重力感应小游戏等。

原先的设想是把这些功能做成 ESP-Brookesia 的 app，由 Brookesia 的桌面统一承载。经过对 Brookesia GUI 接口和系统层的实际代码调研后，放弃这条路线，改为"原生 app + 设备自持镜像库 + 启动切换"。

## 2. 为什么不做 Brookesia 融合

结论：Brookesia 是"应用容器 + 声明式 UI"，不是"实时仪表渲染框架"。三条具体依据：

### 2.1 高频更新多一次异步跳转

Brookesia 的 `set_binding_values` 是异步语义：先入 `pending_gui_bindings_` 缓冲，再由 System worker（默认 poll 2ms、优先级 10）在 GUI task group 里合并 flush。好处是不会积压（最后值胜出），代价是比原生直接调用 `lv_obj_set_x()` 多一次任务跳转，约 2~10ms。

对 30fps 界面可以接受，但对"100Hz 采样 + 30fps 显示"的仪表，等于把延迟预算从 ~35ms 抬到 ~40ms，且抖动变大。

### 2.2 声明式 arc 缺关键能力

ride-gmeter 的双极性弧（6 点钟方向为 0 点，加速向右、刹车向左，各 90°）依赖 `lv_arc_set_angles()` 的任意起止角：

```c
lv_arc_set_angles(s_arc_accel, ARC_BOTTOM_DEG - span, ARC_BOTTOM_DEG);
lv_arc_set_angles(s_arc_brake, ARC_BOTTOM_DEG, ARC_BOTTOM_DEG + span);
```

而 GUI 接口的 `arc` 只有 `rangeProps.value/min/max/step`，没有起止角属性，固定从 0° 起画。替代路径（canvas 每帧序列化 JSON 命令、frameView 每帧 memcpy 整帧）都比原生更慢。

### 2.3 灵敏度参数与帧率耦合

原生 `ui_timer_cb` 每 33ms 执行一次，平滑系数是硬编码的：

```c
s_bx += 0.40f * (tx - s_bx);
s_vert_avg += 0.10f * (ty - s_vert_avg);
s_red_ms += 33;   // 红色保持计时
```

移植后只要更新周期变化，手感就会明显改变。这不是框架的错，但说明"实时仪表"这类 app 需要独占时基控制权。

### 2.4 结论

实时类 app 保持原生；Brookesia 只在"低频工具类"场景有价值（设置、文件、应用商店、语音前端）。两者不需要在同一固件里共存，可以用启动切换并存。

## 3. 切换方式的取舍

| 路线 | 手段 | 耗时 | 前提 | 结论 |
| --- | --- | --- | --- | --- |
| A | 手机 USB-C OTG 直刷（esptool 协议） | 10~20s | 需自写 Android app；通用刷机 App 只认 CH340/CP210x，认不到原生 USB CDC `0x303A:0x1001` | 保留为救砖兜底 |
| B | 设备常驻 recovery + WiFi 网页上传 | 3~6s | 需 recovery 固件与 OTA 服务；救不了 bootloader | 可选补充 |
| C | 设备自持镜像库，本地自刷 + 重启切换 | 2~3s | 分区表 + selector + 共享组件 | **选定** |

不考虑 BLE：4MB 传输需要数分钟，只适合发命令。

## 4. 选定架构

### 4.1 分区表（`partitions/fleet.csv`，32MB Flash）

| 分区 | 类型 | 偏移 | 大小 | 用途 |
| --- | --- | --- | --- | --- |
| `nvs` | data/nvs | 0x9000 | 0x6000 | 标定零偏、配网信息（**保持原偏移与大小，迁移不丢数据**） |
| `phy_init` | data/phy | 0xF000 | 0x1000 | 射频校准 |
| `otadata` | data/ota | 0x10000 | 0x2000 | 启动槽选择 |
| `factory` | app/factory | 0x20000 | 0x200000 (2MB) | selector（常驻，不被覆盖） |
| `ota_0` | app/ota_0 | 0x220000 | 0x800000 (8MB) | 运行槽，任一 app 写这里 |
| `storage` | data/spiffs | 0xA20000 | 0x1500000 (21MB) | 镜像库 |

剩余约 0.9MB 未分配。

### 4.2 为什么是"库 + 单运行槽"，不是"2~6 个槽"

实测 ride-gmeter 的 `build/ride-gmeter.bin` 只有 **723,792 字节（707KB）**，但**不能假设所有 app 都这么小**：带音频解码、FFT、波形渲染或资源打包的 app 可能是 3~6MB，Brookesia 规模的应用更大。

固定多槽的问题就在这里：槽大小必须按最大的 app 规划，小 app 会浪费 80% 空间；而槽数量一旦定死，大 app 就装不下第二个。

改成"库 + 单运行槽"：

- `storage` 里放 **任意数量、任意大小** 的镜像（21MB，按 0.8MB 算约 25 个，或 3 个 6MB 的大应用）；
- 选择后由 selector 从 storage 读出，`esp_ota_write` 到 `ota_0`，`esp_ota_set_boot_partition()`，重启；
- `ota_0` 给到 8MB，覆盖可预见的最大 app；写入 700KB 约 2~3s，8MB 约 15~20s；
- 写入前由 selector 校验 `镜像大小 ≤ 目标分区大小`、`esp_app_desc` 魔数与项目名，超限直接报错而不是写到一半失败。

用户视角完全等价："板子里装了 6 个 app，点一下就换"。只有一种情况会不方便：两个都接近 8MB 的 app 频繁互切，每次要等拷贝。真遇到时把 storage 缩到 13MB、切出 `ota_1` 做 A/B 即可，表结构不用重设计。

### 4.3 镜像库结构（storage 分区，SPIFFS 平铺）

```
/storage
├── manifest.json
├── app-gmeter-1.0.0.bin
├── app-xiaozhi-0.3.1.bin
├── app-musicwave-0.1.0.bin
├── icon-gmeter.png
└── icon-xiaozhi.png
```

- SPIFFS 没有目录，全部平铺，对象名上限 31 字符；
- 选 SPIFFS 而不是 LittleFS：`spiffsgen.py` 随 IDF 发布，PC 端打包零外部依赖；本方案里 storage 是**只读的**（运行时只读镜像，写入只发生在 PC 打包后整分区烧录），LittleFS 的掉电一致性优势用不上。

`manifest.json`：

```json
{
  "version": 1,
  "partition_table": "fleet-1",
  "apps": [
    {
      "id": "gmeter",
      "name": "速度计",
      "version": "1.0.0",
      "bin": "app-gmeter-1.0.0.bin",
      "size": 723792,
      "sha256": "...",
      "icon": "icon-gmeter.png"
    }
  ]
}
```

写入前必须校验：`sha256` → `esp_ota_begin()` 的镜像格式校验 → `esp_app_desc_t.project_name/version` 比对 → `partition_table` 版本比对（防止旧表编译的镜像跑飞）。

### 4.4 启动流程

```
上电
 └─ bootloader → 按 otadata 启动
     ├─ factory(selector)：扫描 storage → 触摸选择 → 校验大小 → 写 ota_0 → 重启
     └─ ota_0(app)：正常运行
          └─ 退出（长按 BOOT / 触摸手势）→ 擦 otadata → 重启回 selector
```

### 4.5 `boot_switch` 共享组件（每个 app 必链）

关键点：bootloader 会直接跳到 `otadata` 指向的槽，**不会经过 factory**。所以"回到 selector"的能力必须存在于每个 app 里，否则一旦进入 app，没有 PC 就回不去。

接口：

```c
esp_err_t boot_switch_init(const boot_switch_config_t *config);  // app_main 最早期调用
esp_err_t boot_switch_mark_boot_ok(void);                        // UI 就绪后调用，清零失败计数
esp_err_t boot_switch_request_selector(void);                    // 切回 selector 并重启
esp_err_t boot_switch_set_boot_slot(const char *label);          // 切到指定 app 分区（"ota_0"）
```

防启动循环：NVS 记 `boot_attempts`，`init` 时 +1，`mark_boot_ok` 清零；连续 3 次未标记成功 → 自动回 factory。

按键语义（以 ride-gmeter 为例）：短按松开（≤1s）= 重新标定；长按 3s = 回 selector。`button_poll()` 只在松开时判断按压时长，长按由 `boot_switch` 在 3s 时刻直接重启，两条路径不会互相触发。1~3s 之间是死区，双方都不响应。

### 4.6 `cjk_font` 共享组件（中文显示）

字体不编进固件，而是打一份 `font-cjk.ttf` 进 `storage`，运行时用 tiny_ttf 从 SPIFFS 读进 PSRAM：

```c
esp_err_t cjk_font_init(void);                              // 挂载 storage 后调用
const lv_font_t *cjk_font_or(int px, const lv_font_t *fb);   // px 档位不存在时返回 fb
```

- 档位按需加载（18/20/26 px 等），每档独立占 PSRAM
- **`LV_SYMBOL_*` 图标不能用这个字体**：CJK TTF 里没有 FontAwesome 字形，必须显式用 `&lv_font_montserrat_xx`
- 好处：selector 和每个 app 共用一份字体，省下 ~2MB 固件空间

### 4.7 `wifi_config` 共享组件（配网状态）

```c
esp_err_t wifi_config_load(wifi_cred_t *out);      // NVS 里的 SSID/密码
esp_err_t wifi_config_save(const char *ssid, const char *pass);
esp_err_t wifi_config_autoconnect(void);           // 后台连接，不阻塞 UI
wifi_config_state_t wifi_config_get_state(void);   // DISCONNECTED / CONNECTING / CONNECTED
const char *wifi_config_get_ip(void);
```

selector 负责配网（扫描 + 键盘），app 只管 `autoconnect()` 与读状态 —— 配网界面只写一份。

### 4.8 `app_settings` 共享组件（跨 app 设置）

selector 负责改、app 负责读，值存在 NVS（namespace `app_cfg`）：

```c
esp_err_t app_settings_init(void);
int  app_settings_get_speaker_volume(void);          // 0..100，默认 60
esp_err_t app_settings_set_speaker_volume(int vol);
```

- 目前只有扬声器音量：selector 设置页滑杆写入，xiaozhi 在 `esp_codec_dev_open()` 之后
  用它设置 `esp_codec_dev_set_out_vol()`，Kconfig 的 `CONFIG_XZ_SPK_VOLUME` 只作默认值
- **滑杆只在松手时 commit**，避免拖动过程中频繁写 flash
- 以后加亮度、自动进上次 app、息屏时间等都在这里扩展，避免每个 app 各存一份

### 4.9 `face_avatar` 共享组件（设备拟人形象 / 表情包）

统一的卡通形象，和字库一样属于**共享资源**，不属于任何 app：素材在
`storage_src/faces/face-<name>.png`，`pack_storage.ps1` 转成
`storage:/face-<name>.bin`（192×192 ARGB8888，约 148 KB/张），运行时读进 PSRAM。

```c
esp_err_t face_avatar_init(void);                                  // 缺 face-idle.bin → ESP_ERR_NOT_FOUND
const lv_image_dsc_t *face_avatar_get(face_avatar_mood_t mood);    // wait/idle/blink/listen/think/sad/happy
int  face_avatar_talk_frames(void);                                // face-talk-1..N.bin 的帧数
const lv_image_dsc_t *face_avatar_talk_frame(int index);
uint16_t face_avatar_size(void);                                   // 边长（192），用于布局
```

- 打包时按不透明像素的包围盒裁剪再等比缩放居中：素材构图各不相同，
  不对齐的话播报换帧时脸会左右跳
- 10 张约占 `storage` 1.4 MB、PSRAM 1.4 MB；换表情只重打 storage，不动 app 镜像
- app 拿不到素材时必须回退到自绘界面，不能开天窗

### 4.10 selector 磁贴图标与标题

- 图标源图两种写法：`icon-<id>.png`（输出 88×88，四周留 8% 边，适合线条图标）、
  `icon-<id>@<px>.png`（输出 `<px>`×`<px>` 满幅 cover，源图自带圆角，用于铺满磁贴）
- selector 按 `icon-<id>.bin` 头里的 `w/h` **1:1 显示、不做缩放**，所以尺寸完全由打包决定；
  磁贴是 132×132，`@125` 即四周各留 3.5 px
- `APP_LOOKS` 里 `title` 写成空串 → 该磁贴不画底部文字，图标垂直居中（有标题时上移 12 px）
- 图标缺失时回退到"烙印汉字/首字母"，不会留空

## 5. 关键风险与对策

| 风险 | 说明 | 对策 |
| --- | --- | --- |
| GPIO0 是 download strap | 上电时按住 BOOT 会进 ROM 下载模式，与"按住切 app"冲突 | 不用"开机按住"；改为 app 运行时长按（3s）触发 `boot_switch_request_selector()` |
| 启动循环 | 新写入的 app 一开机就 panic，会卡死重启 | `boot_attempts` 计数 + 自动回 factory |
| 切 app 丢标定 | 旧的整段镜像写入会覆盖 `nvs` | 分区表保持 `nvs` 偏移/大小不变；日常切换只写 `ota_0` 或 `storage` |
| 开发时烧错槽 | `idf.py flash` 会把 app 写到 `factory`（覆盖 selector） | 开发统一用 `tools/flash_app.ps1 -Slot` 显式指定偏移 |
| 镜像比运行槽大 | 大 app（音频解码、波形）可能超过 8MB | selector 写入前比较 `镜像大小 vs ota_0->size` 并报错；扩容需改表重刷 |
| 镜像不匹配 | 旧分区表编译的 bin 写进新表 | manifest 记 `partition_table` 指纹；selector 另校验 `esp_app_desc` 魔数 |

## 6. 路线图

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| M1 | 统一分区表、`boot_switch` 组件、PC 端打包/烧录脚本 | 已完成 |
| M2 | selector app：SPIFFS 扫描、LVGL 列表、OTA 写入进度 | 已完成（含状态栏、设置页、手势判定） |
| M3 | ride-gmeter 接入 `boot_switch` + 退出入口；第二个 app 接入验证 | 已完成（ride-gmeter + xiaozhi 0.1.4） |
| M4 | （可选）自动启动上次 app、图标与中文名称（manifest） | 待评估 |
| M5 | （可选）Android USB 直刷工具作为救砖兜底 | 待评估 |
| M6 | （可选）Brookesia super 作为 `ota_0` 里的一个镜像接入 | 待评估 |

## 7. 本轮产出

- `partitions/fleet.csv`：统一分区表（唯一事实来源）
- `components/boot_switch/`：启动切换/回退共享组件
- `selector/`：启动选择器（SPIFFS 扫描 + 列表 + OTA 安装 + 进度）
- `tools/flash_app.ps1`：按槽位烧录 app
- `tools/pack_storage.ps1`：扫描镜像目录 → 生成 manifest → `spiffsgen.py` 打包 → 可选烧录
- `ride-gmeter/partitions.csv`：迁移到统一分区表

## 8. 备忘：本方案不解决的问题

- 不能替代 PC 完成首次烧录（bootloader + 分区表仍需一次 esptool）
- selector 自身损坏时，只能用路线 A（手机 USB）或 PC 救砖
- 多 app 共享的 BSP/LVGL 版本仍需人工对齐；manifest 里的 `partition_table` 版本是唯一强制校验，BSP 版本靠约定

## 9. 上板记录（2026-11-18）

环境：ESP32-S3 rev v0.2 / 32MB Flash / 8MB PSRAM / COM3（USB-Serial-JTAG）/ ESP-IDF 6.1。

第一次上板暴露了三个问题，均已修复：

### 9.1 只烧 app 不烧分区表

现象：

```
E esp_image: image at 0x10000 has invalid magic byte (nothing flashed here?)
E boot: Factory app partition is not bootable
```

bootloader 里读到的还是旧布局（`factory 0x10000 4MB`）。补烧 `partition-table.bin` 到 `0x8000` 后正常。已给 `flash_app.ps1` 增加 `-Slot layout`，一次写入 bootloader(`0x0`) + 分区表(`0x8000`)。

### 9.2 SPIFFS 页大小不够

```
E SPIFFS: spiffs partition is too large for spiffs_page_ix type. Please increase CONFIG_SPIFFS_PAGE_SIZE.
```

21MB / 256B = 86,016 页，超过 `spiffs_page_ix`（uint16）的 65,535 上限，挂载直接失败。改为 **page 512**（43,008 页），`sdkconfig.defaults` 与 `pack_storage.ps1` 同步修改。

### 9.3 上电假触摸

触摸控制器（CST9217）初始化时可能给出一次假触摸事件，selector 界面刚出现就收到 `CLICKED`，直接把列表第一项装进 `ota_0` 并重启。已在 `selector_ui.c` 加 800ms 点击屏蔽（`CLICK_GUARD_MS`）。

### 9.4 已验证的闭环

- selector 从 storage 正确挂载 SPIFFS、扫描到 `gmeter 1.0.0`
- 选中后写入 `ota_0` 并 `esp_ota_set_boot_partition`，`otadata` 变为 `ota_seq=1`
- 重启后 ride-gmeter 从 `ota_0` 正常启动：UI ready、50Hz CSV、2 秒心跳 `ok=1002 err=0`

### 9.5 固件输出必须是 ASCII

上板后中文日志与界面文案显示为「口口」——LVGL 的 Montserrat 字体没有 CJK 字形，串口终端的字体/编码也不一定对。约定：

- 固件侧**所有对外字符串**（`ESP_LOGx`、LVGL 文案）只用 ASCII
- 中文只出现在代码注释、PC 端脚本和文档里
- 需要中文界面时，单独打包 CJK 字体子集（体积代价 100KB 量级），不混在默认字体里

### 9.6 待验证

- 长按 BOOT 3 秒返回 selector（`boot_switch` 擦 `otadata` 的路径）
- 掉电重启后仍从 `ota_0` 启动
- 连续 3 次启动失败自动回退 selector

### 9.7 第二个 app 接入（xiaozhi 0.1.3，2026-11-19）

xiaozhi（原生 ESP-IDF 版小智语音助手）成为镜像库里的第二个 app，验证了
「一次打包多 app」的完整链路：

| 步骤 | 命令 | 结果 |
| --- | --- | --- |
| 开发验证 | `tools/flash_app.ps1 -Bin xiaozhi/build/xiaozhi.bin -Slot 0` | 写 `0x220000`，2,113,152 B，哈希校验通过 |
| 发布 | `Copy-Item … storage_src/app-xiaozhi-0.1.3.bin` | sha256 `300e5a6a7afe…` |
| 打包烧录 | `tools/pack_storage.ps1 -Flash -Port COM3` | manifest 记 2 个 app，21 MB 镜像 71.6 s 写完并校验 |

要点：

- **同 id 只能保留一份** `.bin`，换版本时旧文件挪进 `storage_src/_archive/`
  （脚本非递归扫描，`_archive/` 不会被打进镜像，也不参与重复 id 检查）
- 21 MB 镜像实际压缩到 3.0 MB 传输，`-b 921600` 下约 70 s；日常改 app 不必重打，
  只有 app 发布件或共享资源（`font-cjk.ttf`、`faces/`、`icon-*`）变了才需要
- 烧 storage 不影响 `nvs`（配网/标定）和 `ota_0`（当前运行 app），可随时重打

### 9.8 共享表情包与磁贴图标（xiaozhi 0.1.8，2026-11-21）

| 改动 | 要点 |
| --- | --- |
| `components/face_avatar` | `storage_src/faces/face-*.png` → `face-*.bin`（192² ARGB8888）→ PSRAM；10 张占 storage 1.47 MB、PSRAM 1.47 MB |
| 打包 | 按不透明包围盒裁剪 + 等比缩放居中：素材 363~397 见方、构图各异，不对齐的话播报换帧时脸会跳 |
| 渐变背景 | `ui_bg_apply(scr)`：466² RGB565 一张图 434 KB PSRAM，和 selector/watch 同一份实现 |
| 磁贴图标 | `icon-<id>@<px>.png` = 满幅输出（`@125` 铺满 132 磁贴内部），selector 按 bin 尺寸 1:1 显示、不缩放 |
| 无标题磁贴 | `APP_LOOKS.title` 空串 → 不画底部文字，图标垂直居中（有标题时上移 12 px） |

踩坑：图片表情的"呼吸"动画一开始用 `transform_scale`，LVGL 会为 192² 图申请约 74 KB
中间缓冲且落在**内部 RAM**，`[heap] run internal` 从 111 KB 掉到 72 KB、`dma_max` 从
40 KB 掉到 31 KB（低于刷屏所需的 46 KB 连续块）。改用 `LV_OPA` 透明度呼吸后恢复。
**图片对象不要做缩放变换**，用透明度或换帧表达动效。
