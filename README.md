# ESP32 Apps

ESP32 平台上跑的应用、UI 前端与配套资源集合。每个应用一个子目录，自带独立 README。

## 应用列表

| 目录 | 说明 | 目标硬件 | 状态 |
| --- | --- | --- | --- |
| [`selector`](./selector) | 多 app 启动器（常驻 `factory`）：斜向渐变界面 + 顶部状态栏（Wi-Fi / SNTP 时间 / AXP2101 电量）、设置页（Wi-Fi + 存储用量）、拖动/点击手势判定、选中镜像写入 `ota_0` | Waveshare ESP32-S3-Touch-AMOLED-1.75C | 可用 |
| [`xiaozhi`](./xiaozhi) | 小智语音助手（ESP-IDF 原生 C，`esp_xiaozhi` + 板级 BSP）：会话状态机、共享表情包（storage 里的图片表情 + 播报换帧/眨眼）、共享 CJK 字库、selector 同款渐变背景、长按 BOOT 回 selector | Waveshare ESP32-S3-Touch-AMOLED-1.75C | **0.1.8 已进镜像库** |
| [`xiaozhi-ui`](./xiaozhi-ui) | ESP-Brookesia **v0.8** 运行时（JavaScript）应用的"小智"语音助手前端：状态显示、Start/Stop、对话文本、BOOT 键手势 | ESP32-S3 + CO5300 AMOLED 466×466（Waveshare ESP32-S3-Touch-AMOLED-1.75C 实测） | 可用，临时适配 v0.8（被上面的原生版取代） |
| [`ride-gmeter`](./ride-gmeter) | 骑行加速度指示器（ESP-IDF 原生 C）：QMI8658 去重力 + 滤波 + 零偏标定，圆形屏幕气泡界面 + 串口数据流 | Waveshare ESP32-S3-Touch-AMOLED-1.75C（板载 QMI8658） | 阶段二可用（气泡界面） |
| [`watch`](./watch) | 多功能怀表（ESP-IDF 原生 C）：圆形表盘 + 秒表（圈速），NVS 恢复 + SNTP 校时，滑动翻页与 BOOT 键双操作 | Waveshare ESP32-S3-Touch-AMOLED-1.75C | 可用（表盘 + 秒表） |

## 目录约定

- 一个应用一个子目录，目录名用小写加连字符
- 子目录内必须有 `README.md`，写明构建方式、集成方式和已知问题
- `build/` 是否入库由各应用决定：`xiaozhi-ui` 刻意入库，因为 ESP-Brookesia 在 CMake 配置阶段直接 `file(COPY)` 该目录，不会自动调用 `npm run build`
- 不提交 `node_modules/`、`dist/`、`*.bpk`
- 所有应用共用 `partitions/fleet.csv`（镜像切换方案的唯一分区表），不要各自维护
- 应用必须链接 `components/boot_switch`，否则进入应用后无法回到启动选择器
- **背景渐变一律用 `components/ui_bg`**（`ui_bg_apply(obj)`）：30° 斜向渐变图，
  selector 与各 app 共用同一份实现，别在 app 里再写一套
- **中文字库一律用 `components/cjk_font`**（字库文件在 storage 分区，
  由 `tools/pack_storage.ps1` 打包）：`cjk_font_init()` +
  `cjk_font_or(px, &lv_font_montserrat_XX)`。不要在 app 里再 `EMBED_FILES`
  一份 2 MB 字库，也不要依赖 LVGL 内置的 1195 字 CJK 字库（缺字会显示方块）
- **拟人形象/表情一律用 `components/face_avatar`**（素材在 `storage_src/faces/`，
  由 `tools/pack_storage.ps1` 转成 `face-*.bin` 进 storage）：
  `face_avatar_init()` + `face_avatar_get(FACE_AVATAR_XXX)`。别在 app 里各存一套
  表情图，也别为它启用 PNG 解码器（转换在 PC 上做，固件只读裸像素）
- **应用固件必须进 selector 镜像库，禁止用 `idf.py flash` 直接烧**：
  `idf.py flash` 固定写 `0x20000`，那里是 selector，覆盖后长按 BOOT 回不到选择器
  （旧 app 跑在 factory 上还会被 `boot_switch` 误判为「已在 selector」而忽略按键）。
  正确流程见 [`docs/multi-app-image-management.md`](./docs/multi-app-image-management.md)：
  `storage_src/app-<id>-<版本>.bin` → `tools/pack_storage.ps1 -Flash` → 从 selector 启动

## 设计文档

- [多 app 镜像管理方案](./docs/multi-app-image-management.md)：为什么不做 Brookesia 融合、分区表布局、镜像库结构、启动切换流程

## 相关仓库

- ESP-Brookesia 固件工程（提供 `super` 系统、运行时与 `System::dispatch_app_action()` 等原生能力）
- 固件侧的踩坑记录：`examples/system/super/DEVELOPMENT_NOTES.md`
