# XiaoZhi UI 开发进展

> 最后更新：2026-11-15 13:15
> 对应备份：`backup/xiaozhi-ui-20261115-v2`（镜像构建于 2026-11-15 13:12:28）

## 1. 目标与现状

在 ESP-Brookesia 的 super 系统上实现小智（XiaoZhi）AI 助手 UI 示例，适配 **466×466 圆形 AMOLED**（CO5300 + CST9217）。

**当前状态：功能已跑通**。设备可启动应用、激活智能体、发起语音对话，识别文本与回复文本上屏，中英文显示正常。

## 2. 已验证功能

| 功能 | 状态 | 说明 |
|---|---|---|
| 启动 GUI 服务 | ✅ | `brookesia.start_service('SystemGui')` |
| 按钮订阅 | ✅ | `SubscribeAction`：`xz.start` / `xz.stop` |
| AgentManager 启动 | ✅ | 必须 `start_service`，否则函数调用报 `service is not running` |
| Agent 列表 / 选择 | ✅ | `GetAgentNames` → `["Coze","XiaoZhi"]`，`SetTargetAgent('XiaoZhi')` |
| 激活链路 | ✅ | `Activate` → `Activated` → `Start` → `Started`（事件驱动） |
| 语音对话 | ✅ | `audio channel opened` 后自动实时监听 |
| 回复文本上屏 | ✅ | `AgentSpeakingTextGot` → 「小智：…」 |
| 用户识别文本 | ✅ | `UserSpeakingTextGot` → 「你：…」 |
| 监听状态提示 | ✅ | `ListeningStatusChanged` → `正在聆听…` / `点击Start开始说话` |
| 激活码显示 | ✅ 逻辑就绪 | `ActivationCodeReceived` → 中间黄色大字；当前设备已绑定，未触发 |
| 中文渲染 | ✅ | FreeType + NotoSansSC 子集，见第 6 节 |
| 反复进出应用 | ✅ | 修复 `redeclaration` 报错 |

## 3. 关键架构结论（踩坑后确认）

### 3.1 bindable 决定访问方式

| 服务 | bindable | 访问方式 |
|---|---|---|
| `SystemGui` | true | `start_service` 拿句柄 → `call_function` / `subscribe_event` |
| `AgentManager` | true | 同上；**不启动就无法调用任何函数** |
| `XiaoZhi` / `Coze` | **false**（Agent 基类硬编码） | 只能 `call_service_function` / `subscribe_service_event` 按名访问 |

`start_service('XiaoZhi')` 必然失败：`ServiceManager::bind()` 拒绝非 bindable 服务，host_bridge 返回 `Failed to start service: XiaoZhi`。

### 3.2 服务必须先 Running

`call_service_function` 只对已启动的服务有效。早期只按名调用导致：

```
E SvcMgr: [AgentManager:GetAgentNames] : service is not running
```

### 3.3 动作是异步且带前置条件

- `TriggerGeneralAction` 返回 success 仅表示"已接受"，不代表完成
- `Start` 需要 `Activated` 事件就绪，否则 `wait timeout after 1000 ms`
- `Started` 状态下再发 `Activate` 会被状态机拒绝（`No transition for action 'Activate' in state 'Started'`），**但仍返回 success**，因此必须本地记录 `agentState`

### 3.4 GUI 控件路径带容器层级

`SetText` 的 `Path` 是绝对路径，必须包含所有父容器：

```
/main/panel/status  /main/panel/answer  /main/panel/user
/main/panel/code    /main/panel/hint
```

早期写成 `/main/status` 导致 `GUI text view is missing`，界面停在默认文本 `Booting...`。

### 3.5 生命周期与脚本重入

上滑回主菜单会停止应用：

```cpp
// system_navigation.cpp: open_shell_page()
if (active.has_value() && active->app_id != shell_app_id_) {
    return stop_app(active->app_id);
}
```

应用不支持后台驻留。再次启动时运行时会重新求值同一脚本，**顶层 `const`/`let` 会触发 `redeclaration`**，表现为弹错误框并退回主页面。修复方式：整脚本用 IIFE 包裹，只暴露 `globalThis.brookesia_app`。

### 3.6 布局约束

- 骨架只能用 flex 流式；多个 `absolute` 容器混用会导致整体错位（已实测）
- `absolute` 适合单个脱离文档流的元素，例如底部按钮容器：`{"mode":"absolute","align":"bottomMid","y":"-26dp"}`
- `arc` 控件只暴露 `rangeProps` 与 `partStyles`（`arcWidth`/`arcColor`/`arcGradient*`/`arcRounded`），**没有起止角度属性**，无法实现"底部短圆弧按钮"

## 4. 问题时间线（日志时间）

| 时间 | 现象 | 根因 | 修复 |
|---|---|---|---|
| 约 11:09 | `Failed to start service: XiaoZhi` | 设备跑的是旧 bundle，且 `start_service` 用于非 bindable 服务 | 改为按名访问；同步 `build/app/main.js` |
| 约 11:43 | 状态一直 `Booting...` | `SetText` 路径少了 `panel` 层 | 路径改为 `/main/panel/*` |
| 约 11:50 | `service is not running` | AgentManager 未被启动 | `start_service('AgentManager')` |
| 约 11:53 | `Start -> wait timeout` | 连续发送动作，未等状态机事件 | 事件驱动：`Activated` 后才发 `Start` |
| 无时间记录 | 中文显示为方块 | 无中文字体，且 FreeType 未启用 | 放入 `littlefs/system/fonts` + 开启 FreeType/MEMFS |
| 无时间记录 | 反复进出弹 `redeclare` | 脚本顶层 `const` 重入冲突 | IIFE 包裹 |
| 13:1x | Stop 后对话被清空 | 清屏逻辑放在 `Stopped` 分支 | 移到 `Activated` 且 `pendingStart` 时 |

## 5. 关键文件

| 路径 | 作用 |
|---|---|
| `example/xiaozhi-ui/src/app/main.js` | 应用主逻辑（IIFE，事件驱动状态机） |
| `example/xiaozhi-ui/src/res/screens/main.json` | 界面布局（panel 流式 + buttons 绝对定位） |
| `example/xiaozhi-ui/build/` | **设备实际加载的产物**（littlefs stage 的源头） |
| `examples/system/super/main/CMakeLists.txt` | 把 `example/xiaozhi-ui/build` stage 到 `littlefs/apps` |
| `examples/system/super/littlefs/system/fonts/` | 系统字体（index.json + NotoSansSC 子集） |

## 6. 中文字体配置

`sdkconfig` / `sdkconfig.defaults` 需要：

```
CONFIG_ESP_LVGL_ADAPTER_ENABLE_FREETYPE=y
CONFIG_LV_USE_FREETYPE=y
CONFIG_LV_FREETYPE_USE_LVGL_PORT=y
CONFIG_LV_USE_FS_MEMFS=y
CONFIG_LV_FS_MEMFS_LETTER=77
```

`littlefs/system/fonts/index.json` 把 `default` 指向中文子集：

```json
{ "id": "default", "kind": "file", "src": "NotoSansSC-Regular.subset.ttf", "languages": ["en", "zh_CN"] }
```

说明：`CONFIG_SPIRAM_XIP_FROM_PSRAM` 关闭时，`LV_USE_FS_MEMFS` 与 `LV_FREETYPE_USE_LVGL_PORT` 是硬性要求（`style_impl.hpp` 中有 `#error` 守卫）。

## 7. 构建与烧录

```powershell
cd d:/code/esp32_learning/esp-brookesia/examples/system/super
idf.py -p COM3 build flash monitor
```

注意：

- 修改 JS / 布局后，必须先更新 `example/xiaozhi-ui/build`，再 build；否则 stage 的仍是旧内容
- `idf.py build` 会重新执行 `copy_directory` 并重建 `littlefs_data.bin`
- 烧录后开机由 `install_unpacked_apps()` 扫描 `/littlefs/apps` 加载应用
- 设备身份为 MAC，向 `https://api.tenclass.net/xiaozhi/ota/` 查询；已绑定则直接下发 MQTT 配置与 token，无需激活码
- 本机没有 `brookesia` CLI，`dist/*.bpk` 是旧产物；走 littlefs 直装不受影响

## 8. 备份

| 目录 | 内容 |
|---|---|
| `backup/xiaozhi-ui-20261115` | v1：界面最简版（无用户文本、胶囊按钮） |
| `backup/xiaozhi-ui-20261115-v2` | v2：当前版本（IIFE、用户文本、截断、圆形按钮） |

每个备份含 `firmware/`（可直接 esptool 烧录）、`sdkconfig`、`fonts/`、`xiaozhi-ui-src`、`xiaozhi-ui-build` 和 `README.md`。

## 9. 待办 / 可选项

1. 按钮可用性视觉化：GUI 暂无 `SetEnabled`，目前靠状态机拦截并改文案
2. 长回复处理：现在靠字数截断（你 30 字 / 小智 120 字），可考虑固定高度滚动容器
3. 「底部短圆弧」按钮：需给 GUI 后端补 arc 起止角度属性
4. 激活码布局：目前 24sp 居中，若触发需确认与标题、状态的间距
5. `dist/*.bpk` 重新打包（需要 `brookesia` CLI）

## 10. 主屏（super shell）改造规划 — 2026-11-15 评估

目标：圆形屏幕 UI 优化。

| 任务 | 工作量 | 说明 |
|---|---|---|
| 外圈 WiFi 状态 | 0.5 人日 | `overlay.json` 的 `wifi_pill` 从 `status_right` 迁到外圈节点，`shell_status.cpp` 只改路径常量 |
| 外圈电量 | 0.5–1 人日 | AXP2101 已启用，`Device` 服务 `GetPowerBatteryState` + `PowerBatteryStateChanged` 现成 |
| 图标改 gallery 左右滑动 | 1.5–2.5 人日 | `slot_grid` 由 `rowWrap` 改 `row`；`shell_app_launcher.cpp` 改分页 + 订阅 `Gesture`/`Scroll`，复用已有 `grid_x` 绑定做平移动画 |
| 圆屏收尾（避让/动画/边界） | 1 人日 | 注意多个 `absolute` 容器混用会错位 |

合计 3.5–5 人日。

范围决策：

- **蓝牙状态不做**（2026-11-15 确认）。当前 `CONFIG_BT_ENABLED` 未启用，`service/` 下无 bluetooth 组件；开启后 BLE 常驻约 80–120KB 内部 RAM，与语音链路叠加有稳定性风险，故移除该需求。
- 电量环形弧暂缓：`arc` 控件无起止角度属性，做真环形需给 `brookesia_gui_lvgl` 补 `arcStartAngle`/`arcEndAngle`（另 1–2 人日），当前先用图标 + 百分比文字。

现状事实（评估依据）：

- 主屏是 native C++ 应用 `brookesia.system.super.shell`，资源在 `resource/shell/`（31 json + 12 png），改动需重编固件
- 状态栏现有节点：`overlay.json` → `status/status_right/{wifi_pill, clock}`，无 battery 节点
- 图标布局：`app_launcher.json` → `grid_stage(scrollable)` → `slot_grid(flexFlow: rowWrap)`，图标 112×112、间隔 18，列数由容器宽度动态计算
