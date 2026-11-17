# selector

启动选择器，常驻 `factory` 分区（`0x20000`，2MB）。上电后 bootloader 启动它，列出 `storage` 里的镜像，选中后写入运行槽 `ota_0` 并重启。

## 界面

```
        APPS
  ┌────────────────┐
  │ gmeter  1.0.0  │  ← 触摸选择
  │ xiaozhi 0.3.1  │
  │ metronome 0.1  │
  └────────────────┘
   3 个镜像 · 已用 2.1 / 21.0 MB
```

安装时覆盖一层进度浮层；失败时显示原因，触摸浮层返回列表。

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
2. `image_store_init()` 挂载 `storage`（SPIFFS），失败时格式化一次
3. `image_store_scan()` 枚举 `app-*.bin`，校验 `0xE9` 镜像头与 `esp_app_desc_t` 魔数，跳过无效文件
4. 触摸选中 → `image_install_start()`
5. 后台任务校验 `镜像大小 ≤ ota_0 分区大小` → `esp_ota_begin/write/end` → `esp_ota_set_boot_partition(ota_0)` → 重启

## 依赖

- `waveshare/esp32_s3_touch_amoled_1_75c`（BSP，含 LVGL 9.x）
- `../components/boot_switch`（共享组件）
- IDF 自带 `spiffs` / `app_update` / `esp_app_format`

## 已知限制

- 暂无自动启动上次 app 的倒计时（M4）
- 只显示 `esp_app_desc.project_name` 与 `version`，还没用 `manifest.json` 里的中文名和图标
- 镜像大小超过 `ota_0`（8MB）时直接报错，需要改分区表
