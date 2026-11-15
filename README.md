# ESP32 Apps

ESP32 平台上跑的应用、UI 前端与配套资源集合。每个应用一个子目录，自带独立 README。

## 应用列表

| 目录 | 说明 | 目标硬件 | 状态 |
| --- | --- | --- | --- |
| [`xiaozhi-ui`](./xiaozhi-ui) | ESP-Brookesia **v0.8** 运行时（JavaScript）应用的"小智"语音助手前端：状态显示、Start/Stop、对话文本、BOOT 键手势 | ESP32-S3 + CO5300 AMOLED 466×466（Waveshare ESP32-S3-Touch-AMOLED-1.75C 实测） | 可用，临时适配 v0.8 |

## 目录约定

- 一个应用一个子目录，目录名用小写加连字符
- 子目录内必须有 `README.md`，写明构建方式、集成方式和已知限制
- `build/` 是否入库由各应用决定：`xiaozhi-ui` 刻意入库，因为 ESP-Brookesia 在 CMake 配置阶段直接 `file(COPY)` 该目录，不会自动调用 `npm run build`
- 不提交 `node_modules/`、`dist/`、`*.bpk`

## 相关仓库

- ESP-Brookesia 固件工程（提供 `super` 系统、运行时与 `System::dispatch_app_action()` 等原生能力）
- 固件侧的踩坑记录：`examples/system/super/DEVELOPMENT_NOTES.md`
