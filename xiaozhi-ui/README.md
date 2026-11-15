# ESP32 XiaoZhi UI

> ESP-Brookesia **v0.8** 运行时（JavaScript）应用的"小智"语音助手前端，为 **466×466 圆屏** 掌机适配。

本项目是一个**临时适配方案**：ESP-Brookesia v0.8 的 app 运行时接口（服务绑定、事件订阅、GUI binding）还在演进，本项目按 v0.8.x 的实际行为编写，跨大版本升级时需要重新对照 API。

- 框架：ESP-Brookesia `super` 系统（`systems: ["super"]`）
- 依赖服务：`AgentManager` 0.8.0、`XiaoZhi` 0.8.0
- 目标硬件：ESP32-S3 + CO5300 AMOLED 466×466（实测 Waveshare ESP32-S3-Touch-AMOLED-1.75C）

---

## 功能

| 区域 | 内容 |
| --- | --- |
| 顶部 | 状态文本（`Booting...` / `Activating...` / `Listening...` / `Already stopped` 等），字号 24sp |
| 中部 | 绑定码（`code`）、用户识别结果（`user`）、助手回复（`answer`）、操作提示（`hint`） |
| 底部 | 两个 88dp 圆形按钮：`Start` / `Stop`，间距 48dp |
| 硬件手势 | 双击板载 BOOT 键 = `app.toggle`（运行中则停止，否则启动），由原生层通过 `System::dispatch_app_action()` 派发 |

界面不含标题栏：状态文本即主视觉，聊天文本已放大，以适配圆屏可见区域。

## 目录结构

```
.
├─ brookesia.config.js      # CLI 配置：src → build，dist 为 release，PNG 转换目录
├─ package.json             # npm scripts 包装 brookesia CLI
├─ assets/images/           # 可选的图片转换输入（当前未启用）
├─ src/                     # 源码
│  ├─ manifest.json         # 包 id / 入口 / 所需服务
│  ├─ app/main.js           # 运行时逻辑（IIFE 包裹，防止重复求值报 redeclaration）
│  └─ res/                  # root.json / profile.json / flows / screens / images
├─ build/                   # 构建产物（已入库，见下文）
└─ dist/                    # .bpk 发布包（未入库）
```

`build/` 刻意保留在版本库里：ESP-Brookesia 的 CMake 在 **configure 阶段** 直接从这个目录 `file(COPY)` 到 littlefs，不会自动调用本项目的构建脚本。把它入库可以让 `git clone` 后立即接入固件工程。

## 构建

前置：Node.js + `brookesia` CLI 已加入 PATH（随 ESP-Brookesia v0.8 工具链提供）。

```bash
npm run doctor     # 检查 CLI 与工程配置
npm run build      # src → build（含 PNG → LVGL .bin 转换）
npm run release    # 生成 dist/esp.brookesia.app.xiaozhi.<version>.bpk
npm run simulate   # 本地模拟运行（需 CLI 支持）
```

图片转换配置在 `brookesia.config.js`：

```js
convertImages: 'res/images',   // 相对 src，PNG 会转成 build/res/images/<id>.bin
```

改完 `src/` 后**必须重新 `npm run build`**，`build/` 不会自动同步。

## 集成到 ESP-Brookesia 固件

以 `esp-brookesia/examples/system/super` 为例，`main/CMakeLists.txt` 里有：

```cmake
set(XIAOZHI_APP_PACKAGE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../../example/xiaozhi-ui/build")
if(COMMAND brookesia_stage_runtime_app_package AND EXISTS "${XIAOZHI_APP_PACKAGE_DIR}/manifest.json")
    brookesia_stage_runtime_app_package(
        PACKAGE_ID esp.brookesia.app.xiaozhi
        SOURCE_DIR "${XIAOZHI_APP_PACKAGE_DIR}"
        STAGE_ROOT "littlefs/apps"
    )
endif()
```

注意两点：

1. **只有 `manifest.json` 存在才会 stage**，否则只打印 WARNING，设备上跑的是 littlefs 里的旧包。
2. stage 是 CMake 配置阶段的 `file(COPY)`，所以直接改 `littlefs/apps/...` 会在下次 configure 时被覆盖。**真源只在本仓库的 `src/`（构建后进 `build/`）**。

改完 UI 后，最小生效路径：

```powershell
npm run build
# 然后重新构建固件
idf.py -C <固件工程> build
idf.py -C <固件工程> -p COM3 flash
```

## 运行时契约

### 动作（`on_action`）

| action | 行为 |
| --- | --- |
| `xz.start` | 激活并开始对话；已在 `Started` 则重新开始聆听 |
| `xz.stop` | 停止当前会话 |
| `xz.listen` | 仅触发 `ManualStartListening` |
| `app.toggle` | 硬件手势：`Started` → 停止，否则启动 |

未识别的 action 一律忽略，因此新增手势不需要改其它 app。

### GUI 绑定路径

```
/main/panel/status   状态文本
/main/panel/code     绑定码
/main/panel/user     用户识别结果
/main/panel/answer   助手回复
/main/panel/hint     操作提示
```

`res/screens/main.json` 调整布局时，路径必须同步修改 `src/app/main.js` 顶部的 `GUI_*` 常量。

### 服务访问方式

- `AgentManager`：`bindable = true`，需 `start_service()` 拿到 handle，之后每次调用都要求服务在运行
- `XiaoZhi` / `Coze`：`bindable = false`，由 AgentManager 拥有，只能按名字 `subscribe_service_event`

`main.js` 用顶层注释声明服务依赖（`// @brookesia/service/agent-manager`），CLI 扫描后写入 `manifest.services`；**不要改成 `import`**，否则打包后会保留模块路径，设备端运行时会尝试加载失败。

## 已知限制

- 仅验证 `en` 语言资源，未做多语言
- `agentState` 依赖 `AgentManager` 的事件字符串（`Activating` / `Started` / `Stopped` / `Ready`），框架改名就会失效
- 没有 `package-lock.json`，CLI 版本未锁定
- `build/` 入库意味着每次改源码都要提交产物，历史里会出现二进制 diff
- 硬件手势依赖固件侧存在 `System::dispatch_app_action()` 与 BOOT 键映射，纯应用侧无法单独启用

## 相关项目

- ESP-Brookesia 固件工程（含 `super` 系统与本应用的 stage 配置）
- 开发过程中的踩坑记录见固件仓库 `examples/system/super/DEVELOPMENT_NOTES.md`

## License

未指定，默认保留所有权利。如需开源请补充 LICENSE。
