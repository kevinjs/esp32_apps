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
| `factory` | app/factory | 0x20000 | 0x300000 | selector（常驻，不被覆盖） |
| `ota_0` | app/ota_0 | 0x320000 | 0x400000 | 运行槽，任一 app 写这里 |
| `storage` | data/littlefs | 0x720000 | 0x1400000 (20MB) | 镜像库、图标、资源 |

剩余约 4.9MB 未分配，留给后续扩展（第二个槽或第二块 storage）。

### 4.2 为什么是"库 + 单运行槽"，不是"2~6 个槽"

实测 ride-gmeter 的 `build/ride-gmeter.bin` 只有 **723,792 字节（707KB）**。即使加上 LVGL、BSP、字体，原生 app 通常在 0.7~1.5MB。

因此"多槽"带来的收益（切换不用拷贝）远小于成本（槽位管理、"哪个槽空闲"、每槽 4MB 浪费）。改成：

- 20MB 的 `storage` 里放 **任意数量** 的 app 镜像（按 0.8MB 算约 25 个）；
- 选择后由 selector 从 storage 读出，`esp_ota_write` 到 `ota_0`，`esp_ota_set_boot_partition()`，重启；
- 写入 700KB 约 2~3s，用户感知与"秒切"差距很小。

用户视角完全等价："板子里装了 6 个 app，点一下就换"。如果以后确实需要瞬时切换，把 storage 缩到 16MB、切出 `ota_1` 即可，表结构不用重设计。

### 4.3 镜像库结构（storage 分区）

```
storage/
├── manifest.json
├── app-gmeter-1.0.0.bin
├── app-xiaozhi-0.3.1.bin
├── app-metronome-0.1.0.bin
└── icons/
    ├── gmeter.png
    └── xiaozhi.png
```

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
      "icon": "icons/gmeter.png"
    }
  ]
}
```

写入前必须校验：`sha256` → `esp_ota_begin()` 的镜像格式校验 → `esp_app_desc_t.project_name/version` 比对 → `partition_table` 版本比对（防止旧表编译的镜像跑飞）。

### 4.4 启动流程

```
上电
 └─ bootloader → 按 otadata 启动
     ├─ factory(selector)：扫描 storage → 显示列表 → 3s 倒计时自动启动上次 app
     │    └─ 选中 → 校验 → 写 ota_0 → set_boot(ota_0) → 重启
     └─ ota_0(app)：正常运行
          └─ 退出（长按/触摸手势）→ set_boot(factory) → 重启
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

## 5. 关键风险与对策

| 风险 | 说明 | 对策 |
| --- | --- | --- |
| GPIO0 是 download strap | 上电时按住 BOOT 会进 ROM 下载模式，与"按住切 app"冲突 | 不用"开机按住"；改为 app 运行时长按（3s）触发 `boot_switch_request_selector()` |
| 启动循环 | 新写入的 app 一开机就 panic，会卡死重启 | `boot_attempts` 计数 + 自动回 factory |
| 切 app 丢标定 | 旧的整段镜像写入会覆盖 `nvs` | 分区表保持 `nvs` 偏移/大小不变；日常切换只写 `ota_0` 或 `storage` |
| 开发时烧错槽 | `idf.py flash` 会把 app 写到 `factory`（覆盖 selector） | 开发统一用 `tools/flash_app.ps1 -Slot` 显式指定偏移 |
| LittleFS 掉电 | 反复写镜像文件，SPIFFS 掉电一致性差 | `storage` 用 `littlefs` 子类型 |
| 镜像不匹配 | 旧分区表编译的 bin 写进新表 | manifest 记 `partition_table` 版本，selector 拒绝不匹配项 |

## 6. 路线图

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| M1 | 统一分区表、`boot_switch` 组件、PC 端打包/烧录脚本 | 本轮完成 |
| M2 | selector app：LittleFS 扫描、LVGL 列表、OTA 写入进度、倒计时启动 | 待开始 |
| M3 | ride-gmeter 接入 `boot_switch` + 退出入口；第二个 app 接入验证 | 待开始 |
| M4 | （可选）Android USB 直刷工具作为救砖兜底 | 待评估 |
| M5 | （可选）Brookesia super 作为 `ota_0` 里的一个镜像接入 | 待评估 |

## 7. 本轮产出

- `partitions/fleet.csv`：统一分区表（唯一事实来源）
- `components/boot_switch/`：启动切换/回退共享组件
- `tools/flash_app.ps1`：按槽位烧录 app
- `tools/pack_storage.ps1`：扫描镜像目录 → 生成 manifest → 打包 LittleFS → 可选烧录
- `ride-gmeter/partitions.csv`：迁移到统一分区表

## 8. 备忘：本方案不解决的问题

- 不能替代 PC 完成首次烧录（bootloader + 分区表仍需一次 esptool）
- selector 自身损坏时，只能用路线 A（手机 USB）或 PC 救砖
- 多 app 共享的 BSP/LVGL 版本仍需人工对齐；manifest 里的 `partition_table` 版本是唯一强制校验，BSP 版本靠约定
