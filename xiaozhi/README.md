# xiaozhi（原生 ESP-IDF 版）

小智语音助手，直接用 `espressif/esp_xiaozhi` + 板级 BSP 实现，**不依赖 ESP-Brookesia**。
与 `ride-gmeter` 平级：编译成普通 app 镜像，由 `selector` 装进 `ota_0` 启动，长按 BOOT 3 秒回到选择界面。

## 结构

```
xiaozhi/
├─ CMakeLists.txt          # 只挂 ../components（boot_switch）
├─ partitions.csv          # fleet.csv 副本，方案 A 不改分区表
├─ sdkconfig.defaults
└─ main/
   ├─ Kconfig.projbuild    # Wi-Fi 账号、BOOT GPIO、麦克风增益、音量
   ├─ idf_component.yml    # BSP + esp_xiaozhi + esp_audio_codec
   ├─ main.c               # 启动编排 + 按钮命令队列
   ├─ xz_wifi.c            # Wi-Fi STA
   ├─ xz_audio.c           # ES7210/ES8311 + Opus 编解码 + 收发任务
   ├─ xz_chat.c            # 会话生命周期，协议事件 -> UI
   └─ xz_ui.c              # 状态 + 详情 + TALK 按钮
```

## 配置

```powershell
idf.py menuconfig
# Xiaozhi app -> Wi-Fi SSID / password
```

或直接改 `sdkconfig.defaults` 之前先执行一次 `idf.py menuconfig` 写入 `sdkconfig`（`sdkconfig` 不入库）。

## 构建与烧录

```powershell
cd d:\code\esp32_learning\esp32_apps\xiaozhi
. $env:IDF_PATH\export.ps1        # 或已导出的 IDF 终端
idf.py build

# 装进镜像库（selector 列表里会出现 xiaozhi）
Copy-Item .\build\xiaozhi.bin ..\storage_src\app-xiaozhi-0.1.0.bin
..\tools\pack_storage.ps1

# 只更新 selector 的镜像库分区，不动 app
```

首次启动后会显示 6 位激活码，到 <https://xiaozhi.me> 绑定设备。

## 交互

| 操作 | 行为 |
| --- | --- |
| 点屏幕底部 TALK | 开始 / 停止一轮对话 |
| 长按 BOOT 3 秒 | 回到 selector |

## 音频参数

| 项 | 值 |
| --- | --- |
| 采样率 | 16000 Hz |
| 声道 / 位宽 | 单声道 / 16 bit |
| Opus 帧长 | 60 ms（与服务端 hello 协商一致） |
| 码率 | 24 kbps，complexity 3 |
| 模式 | 半双工，TTS 播放时暂停上行 |

## 已知限制

- 没有唤醒词（ESP-SR WakeNet 需要额外 `model` 分区，见方案 B）
- 没有 AEC，播放时靠半双工规避回声
- 不做设备端 MCP 控制（`cfg.mcp_engine = NULL`）
- Wi-Fi 账号走 menuconfig，未做配网 UI
- BOOT 短按未绑定（避免与 `boot_switch` 的长按检测抢同一个 GPIO）
