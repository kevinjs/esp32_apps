# xiaozhi（原生 ESP-IDF 版）

小智语音助手，直接用 `espressif/esp_xiaozhi` + 板级 BSP 实现，**不依赖 ESP-Brookesia**。
与 `ride-gmeter` 平级：编译成普通 app 镜像，由 `selector` 装进 `ota_0` 启动，长按 BOOT 3 秒回到选择界面。

当前发布版：**0.1.8**（`storage_src/app-xiaozhi-0.1.8.bin`，2,117,824 B，sha256 前缀 `e76ca2e56305`）。
版本号只体现在镜像库文件名里，代码里没有版本宏；发新版时同步改这里和 `storage_src/README.md`。

## 结构

```
xiaozhi/
├─ CMakeLists.txt          # 挂 ../components（boot_switch / cjk_font / face_avatar / ui_bg / app_settings / wifi_config）
├─ partitions.csv          # fleet.csv 副本，方案 A 不改分区表
├─ sdkconfig.defaults
└─ main/
   ├─ Kconfig.projbuild    # Wi-Fi 账号、BOOT GPIO、麦克风增益、音量
   ├─ idf_component.yml    # BSP + esp_xiaozhi + esp_audio_codec
   ├─ main.c               # 启动编排 + 按钮命令队列
   ├─ xz_wifi.c            # Wi-Fi STA
   ├─ xz_audio.c           # ES7210/ES8311 + Opus 编解码 + 收发任务
   ├─ xz_chat.c            # 会话生命周期，协议事件 -> UI
   └─ xz_ui.c              # 状态文案 + 共享表情包（图片）+ 详情 + TALK 按钮
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

# 固件 2.1 MB > factory 的 2 MB，必须烧到 ota_0(8MB) 并切启动分区。
# 日常调试（不经过 selector，最快）：
.\flash_dev.ps1 -Port COM3

# 正式流程（进镜像库，selector 列表里出现 xiaozhi）：
# 1) 先把镜像写进 ota_0 上机验证
..\tools\flash_app.ps1 -Bin build\xiaozhi.bin -Slot 0
# 2) 确认没问题再发布：旧版本挪进 _archive（同 id 只能留一份）
New-Item -ItemType Directory ..\storage_src\_archive -Force | Out-Null
Move-Item ..\storage_src\app-xiaozhi-<旧版本>.bin ..\storage_src\_archive\ -Force
Copy-Item build\xiaozhi.bin ..\storage_src\app-xiaozhi-0.1.8.bin -Force
# 3) 重写 21MB 镜像库（约 70s，只动 storage 分区）
..\tools\pack_storage.ps1 -Flash
```

打包日志里会打印每个 app 的 size 和 sha256 前 12 位，和 `storage_src/README.md`
的「当前镜像库」表对一遍即可（0.1.8 = 2,117,824 B / `e76ca2e56305`）。

> **不要用 `idf.py flash`**：它固定把镜像写到 `0x20000`（factory），会**覆盖 selector**。
> 一旦 selector 被覆盖，长按 BOOT「回 selector」只会回到 factory 里那份旧的 xiaozhi，
> 而且旧 app 跑在 factory 上会被 `boot_switch` 当成 selector（`subtype == factory`），
> 之后再长按 BOOT 直接被忽略。
>
> 恢复：`..\tools\flash_app.ps1 -Bin ..\selector\build\selector.bin -Slot selector`，
> 然后擦掉 otadata 让它回落 factory：
> `python -m esptool --chip esp32s3 -p COM3 erase-region 0x10000 0x2000`

首次启动后会显示 6 位激活码，到 <https://xiaozhi.me> 绑定设备。

## 交互

只有一个按钮，文案随状态变化，灰色 = 当前不可点。

| 会话状态 | 按钮 | 颜色 | 点击行为 |
| --- | --- | --- | --- |
| `IDLE` | 说话 | 蓝 | `listen start`(manual) + 开麦 |
| `LISTENING` | 说完 | 绿 | `listen stop` + 停采集，等服务端识别 |
| `THINKING` | 取消 | 灰蓝 | 放弃本次识别，立即回 `IDLE` |
| `SPEAKING` | 打断 | 橙 | `abort` + 清空播放队列 |
| `WIFI`/`AUDIO`/`LINK`/`CONNECTING`/`CONNECTED` | 联网中/初始化/连接中/准备中 | 灰 | 不可点 |
| `DISCONNECTED`/`NO WIFI`/`ERROR`/`SERVER ERROR` | 重试 | 蓝 | 重新 `start_session` |

长按 BOOT 3 秒回 selector。

一次完整的对话：

```
点击(说话) ──► listen start(manual) ──► 上行 Opus
点击(说完) ──► listen stop ──► 服务端 stt ──► llm ──► tts ──► 播报完回到 IDLE
```

**关键点**：`manual` 模式下服务端不做 VAD，**必须**收到 `listen stop` 才开始识别；
只发 `start` 会一直收音频但永远没有结果，约 60 秒后服务端回 `goodbye`。

### 背景

整屏背景用共享组件 `components/ui_bg` 铺 **selector 同款 30° 斜向渐变**
（`#07070E` 顶部近黑 → `#1A1230` 右下暗紫）：466×466 RGB565 在 PSRAM 里只生成
一次（434 KB），与 selector、watch 视觉完全一致。生成失败（PSRAM 不足）时退化为
深色纯色，不会白屏。

表情是透明背景的 PNG，直接叠在渐变上，不需要额外的底板或描边。

### 表情（设备统一的拟人形象）

界面中部是一张**会动的卡通脸**，素材是全设备共用的表情包，放在 `storage` 分区
（`face-*.bin`，192×192 ARGB8888），由共享组件 `components/face_avatar` 读进 PSRAM。
素材规格与打包流程见 [`../storage_src/faces/README.md`](../storage_src/faces/README.md)。

| 会话状态 | 表情 | 动效 |
| --- | --- | --- |
| `BOOTING`/`WIFI`/`AUDIO`/`LINK`/`CONNECTING`/`CONNECTED` | `face-wait` 等待脸 | 透明度呼吸（80%↔100%） |
| `IDLE` | `face-idle` 微笑 | 呼吸 + 每 3~6 s 眨一次眼 |
| `LISTENING` | `face-listen` 惊讶听 | 呼吸 |
| `THINKING` | `face-think` 思考 | 呼吸 |
| `SPEAKING` | `face-talk-1..3` 口型循环 | 200 ms 换一帧，常亮不叠动效 |
| `DISCONNECTED`/`NO WIFI`/`ERROR`/`SERVER ERROR` | `face-sad` 沮丧 | 呼吸 |

**为什么呼吸用透明度而不是缩放**：`transform_scale` 要实时重采样，LVGL 会给
192×192 的图申请约 74 KB 中间缓冲且落在**内部 RAM**，实测把 `dma_max` 从
137 KB 压到 31 KB（刷屏中转缓冲只要 46 KB 就见底，久了残影/停更）。
改成 `LV_OPA` 后在目标缓冲上逐像素混合，不额外占内存：`internal` 72 KB → 111 KB。

`xz_ui_add_message()` 保留接口但**不上屏**，识别/播报文本只进串口日志。

**为什么不上文字**：中文正文由 tiny_ttf 渲染，每遇到一个新汉字就缓存一份字形位图
（上限 `CONFIG_LV_TINY_TTF_CACHE_GLYPH_CNT`=512），十几轮就能把内部 RAM 吃干；
随后 `esp-aes` 连 200 字节的解密 DMA 缓冲都申请不到，会话永久卡在 thinking→idle。
表情方案只渲染固定文案，字形数量有界。

**回退**：`face_avatar_init()` 失败（storage 没刷、`face-idle.bin` 缺失）时，
界面自动退到 `xz_ui.c` 里那套 `lv_obj` 几何简笔脸，启动日志会打
`shared avatar unavailable, using built-in face`。换表情只需重打 storage，
**不用重编这个 app**。

selector 里 xiaozhi 磁贴的图标也是从同一张 `face-idle.png` 生成的
（`tools/make_app_icon.ps1 -NoCard -RadiusRatio 0.24` → `icon-xiaozhi@125.png`，
125×125 铺满磁贴），列表和 app 内是同一个形象。

### 卡在「识别中」的两道兜底

1. **空音频直接收尾**：`listen` 期间上传不足 2 帧（≈120 ms，即点开始后立刻点停止）
   时不发识别请求，本地直接回 `IDLE` 并提示「没听到声音，再说一次」
2. **8 秒超时**：发了 `listen stop` 后起一个 `esp_timer`；期间收到
   `stt` 文本或 `speech_started` 就取消，超时则回 `IDLE` 并提示「没听清，再说一次」

`THINKING` 阶段按钮变成可点的「取消」，用户不想等也能立刻退出，不必等超时。

## 音频参数

| 项 | 值 |
| --- | --- |
| 采样率 | 16000 Hz |
| 声道 / 位宽 | 单声道 / 16 bit |
| Opus 帧长 | 60 ms（与服务端 hello 协商一致） |
| 码率 | 24 kbps，complexity 3 |
| 模式 | 半双工，TTS 播放时暂停上行 |
| 麦克风 | ES7210，MIC1+MIC2，增益 `CONFIG_XZ_MIC_GAIN_DB`(24 dB) |
| 扬声器 | ES8311，音量 `CONFIG_XZ_SPK_VOLUME`(70) |

## 内存约束（踩过的坑，改代码前先看）

这块板子内部 RAM 很紧张，LVGL 双缓冲放在 PSRAM 里，**每次刷屏都要在内部 RAM
分配一块同尺寸的 DMA 中转缓冲**（`buffer_height=50` → 466×50×2 ≈ 46 KB），
SPI2 DMA 访问不了 PSRAM。内部 RAM 一旦被别的模块吃光，表现是：

- 屏幕停止刷新：`spicommon_dma_setup_priv_buffer: Failed to allocate priv TX buffer`
- TLS 建链失败：`mbedtls_ssl_setup returned -0x008D`（`MBEDTLS_ERR_SSL_ALLOC_FAILED`）

因此：

| 约束 | 做法 |
| --- | --- |
| 大栈 | `xz_cap` 40 KB、`xz_play` 32 KB，用 `xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM)` 放 PSRAM；退出必须 `vTaskDeleteWithCaps` |
| PCM 缓冲 | 用 `heap_caps_malloc(MALLOC_CAP_INTERNAL\|MALLOC_CAP_8BIT)` 显式放内部 RAM，不能跟着栈落在 PSRAM（I2S DMA 读不到 PSRAM，会出现 `peak 32768` 尖峰/杂音） |
| 内部 RAM 预留 | `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536`（默认 32 KB 不够） |
| 启动打点 | `main.c` 的 `log_heap()` 在 start/audio/chat 三处打印 `internal/dma/psram` |
| 共享资源 | 字库 ~2 MB + 表情包 ~1.4 MB（10 张 192² ARGB8888）+ 背景渐变 434 KB，都在 PSRAM，不吃内部 RAM |

启动基线（正常值）：`internal≈145 KB dma≈137 KB`，单次刷屏需要 46 KB。

## 中文显示（共享字库）

中文用 **tiny_ttf 运行时渲染**，字库**不在 app 镜像里**，而是放在 `storage`
分区的 `font-cjk.ttf`，由共享组件 `components/cjk_font` 读进 PSRAM：

| 环节 | 说明 |
| --- | --- |
| 字库文件 | `storage:/font-cjk.ttf`（GB2312 全集子集，2.0 MB，7542 字） |
| 生成 | `python scripts/gen_cjk_font.py`（系统黑体 + fontTools 裁剪，直接输出到 `../storage_src/`） |
| 打包 | `tools/pack_storage.ps1` 把 `storage_src/font-*.ttf` 打进 storage 分区 |
| 运行 | `cjk_font_or(18, ...)` / `cjk_font_or(26, ...)`，见 `xz_ui.c: fonts_init()` |
| 兜底 | storage 字库缺失 → 内置 1195 字位图字库 → Montserrat |

selector 与所有 app 共用同一份字库（selector 的 factory 分区只有 2 MB，装不下），
所以**换字体/扩字符集只需要重新生成 + 重打 storage，不用重编任何 app**。

本工程需要的配置：

- `CONFIG_LV_USE_TINY_TTF=y`
- LVGL 从内置 64 KB 内存池切到系统堆：`CONFIG_LV_USE_CLIB_MALLOC=y`
  （`CONFIG_LV_USE_STDLIB_MALLOC=1`），否则整屏中文 + 字形缓存会撑爆内存池
- `CONFIG_SPIFFS_PAGE_SIZE=512` —— 和 `pack_storage.ps1` 必须一致；
  用默认 256 时 21 MB 分区 mount 会直接返回 `ESP_ERR_INVALID_ARG`
  （页数 86016 超过 `spiffs_page_ix` 的 65535 上限）

固件因此回到 2.1 MB（原来嵌字库是 4.0 MB），但仍超过 factory 的 2 MB，继续走 `ota_0`。

## 调试

```powershell
# 抓串口（UTF-8，中文不会变 ?）
powershell -File d:\code\esp32_learning\tmp\serial_mon.ps1 -Port COM3 -Seconds 180 `
           -Out d:\code\esp32_learning\tmp\xz_serial.log
```

固件里的诊断日志：

| 日志 | 含义 |
| --- | --- |
| `[heap] chat internal=.. dma=.. psram=..` | 各阶段内部/DMA/PSRAM 余量 |
| `[heap] run internal=.. dma_max=..` | 每 5 s 一行常驻打点；`dma_max` 见底就解不出音频包，会话接不上 |
| `[heap] rx N int=.. dma=.. psram=..` | 每收到一个音频包打一行 |
| `face_avatar: avatar ready: 192x192` | 共享表情包加载成功；跟着的 `talk frames: 3` 是播报帧数 |
| `xz.ui: shared avatar unavailable, using built-in face` | storage 里没有 `face-idle.bin`，界面退回几何简笔脸 |
| `xz.audio: mic tx 34 frames / 2038ms, peak N` | 每 2 s 一帧计数 + 麦克风峰值；静音 <100，说话 400~1500，`32768` 说明缓冲/增益有问题 |
| `xz.chat: talk start` / `talk stop, waiting for stt` | manual 模式的上行起点/终点 |
| `xz.chat: user: ...` / `assistant: ...` | 服务端 stt / llm 文本 |
| `***ERROR*** A stack overflow in task xz_play` | 播放栈不够（已从 12 KB 提到 32 KB） |

## 已知限制

- 中文字库是 GB2312 子集（7542 字）：GB2312 之外的生僻字、Emoji 仍不显示；
  要全 Unicode 就把 `scripts/gen_cjk_font.py` 的字符范围换成 GBK/全字库
  （体积会涨到 5 MB 以上，注意 8 MB 分区余量）
- 字库由 `cjk_font` 读进 PSRAM（约 2 MB），PSRAM 可用堆 6.2 MB → 4.1 MB
- 字库在 `storage` 分区：如果 storage 被格式化 / 没打镜像库，
  中文会退化成 1195 字位图字库（部分方块），日志会有
  `cjk_font: /storage/font-cjk.ttf not found`
- `THINKING` 最长等 8 s（`xz_chat.c` 的 `THINK_TIMEOUT_MS`）就回 `IDLE`，期间可点「取消」主动退出；
  真正等到服务端 60 s `goodbye` 的情况已经不会出现了
- 对话内容**不上屏**（只有表情），识别/播报文本只能在串口日志里看
- 没有唤醒词（ESP-SR WakeNet 需要额外 `model` 分区，见方案 B）
- 没有 AEC，播放时靠半双工规避回声
- 设备端 MCP 只创建了空 engine（`esp_mcp_create`），未注册任何工具
- Wi-Fi 账号走 menuconfig，未做配网 UI
- BOOT 短按未绑定（避免与 `boot_switch` 的长按检测抢同一个 GPIO）

## 变更记录

**2026-11-21（发布 0.1.8）**
- 背景从纯黑换成 **selector 同款 30° 斜向渐变**：直接用共享组件 `ui_bg`，
  466×466 RGB565 一张图铺 `bg_image`（434 KB PSRAM），与 selector / watch 一致
- 表情素材本身是透明底，直接叠在渐变上，不需要底板或描边
- 固件 2,117,824 B；PSRAM 余量 3.33 MB → 2.89 MB，内部 RAM 不变（110 KB）
- 发布件 `storage_src/app-xiaozhi-0.1.8.bin`（sha256 `e76ca2e56305…`），
  0.1.7 归档到 `_archive/`

**2026-11-21（发布 0.1.7）**
- 表情换成**设备统一的拟人形象**：素材在 `storage_src/faces/`（10 张 PNG），
  打包成 `storage:/face-*.bin`（192×192 ARGB8888，1.4 MB），由新增的共享组件
  `components/face_avatar` 读进 PSRAM，selector 与其他 app 也能直接用
- `xz_ui.c` 改用图片表情：播报 3 帧轮播（200 ms/帧）、待命随机眨眼（3~6 s）、
  静态状态透明度呼吸；素材缺失时自动回退到原来的几何简笔脸
- `tools/pack_storage.ps1` 新增 `faces/face-*.png → face-*.bin`：按不透明包围盒
  裁剪 + 等比缩放居中（素材 363~397 见方、构图各异，不裁对齐换帧时脸会跳）
- 修内存：呼吸动画初版用 `transform_scale`，LVGL 为 192² 图申请约 74 KB 中间缓冲
  （内部 RAM），实测 `[heap] run internal` 111 KB → 72 KB、`dma_max` 40 KB → 31 KB，
  低于刷屏所需的 46 KB 连续块；改用 `LV_OPA` 后 internal 回到 111 KB
- 固件 2,117,184 B（+2.4 KB），仍走 `ota_0`
- 发布件 `storage_src/app-xiaozhi-0.1.7.bin`（sha256 `1d9b5f43eeca…`）；
  0.1.5 与过渡版 0.1.6（带缩放呼吸的那次构建）归档到 `_archive/`

**2026-11-21（发布 0.1.5）**
- 界面中部“可滑动对话文字稿”换成**会动的表情脸**：眨眼 + 呼吸缩放 + 嘴型随状态变
  （等 / 待命 / 听 / 想 / 说 / 出错），纯 `lv_obj` 绘制，不加载图片
- 修多轮对话后会话卡死：中文正文每轮都给 tiny_ttf 新增字形缓存（512 上限），
  吃干内部 RAM 后 `esp-aes` 分配解密 DMA 缓冲失败；现在只渲染固定文案，字形数量有界
- 内存可观测性：`xz_chat.c` 每收到一个音频包打 `[heap] rx N int/dma/psram`；
  `main.c` 新增 `heap_monitor_task`，每 5 s 打一行 `[heap] run ...`
  （含 `dma_max` 最大连续块，区分“内存见底”与“只是碎片化”）
- 发布件 `storage_src/app-xiaozhi-0.1.5.bin`（2,114,816 B，sha256 `47cd055fc0de…`），
  0.1.4 归档到 `_archive/`

**2026-11-20（发布 0.1.4）**
- 扬声器音量改由 selector 设置页统一管理：启动时读 `components/app_settings`
  （NVS `app_cfg/spk_vol`），Kconfig 的 `CONFIG_XZ_SPK_VOLUME` 只作默认值
- 发布件 `storage_src/app-xiaozhi-0.1.4.bin`（2,113,520 B，sha256 `f5d044f3ddf6…`），
  0.1.3 归档到 `_archive/`

**2026-11-19（发布 0.1.3）**
- 界面定版，发布进 selector 镜像库：`storage_src/app-xiaozhi-0.1.3.bin`
  （2,113,152 B，sha256 `300e5a6a7afe…`），旧 0.1.2 归档到 `storage_src/_archive/`
- 上机验证：启动无 `E (...)`，`cjk_font` 18/26 px 就绪、`ui ready`、
  Wi-Fi 连上、`chat ready (mqtt=1 ws=1)`

**2026-11-19（深夜 2）**
- 界面中部改成可滑动对话区：保留最近 12 条「我 / 小智」消息，触摸上下翻看
- 修「识别中」卡死：空音频（<2 帧）本地直接收尾 + 8 s 识别超时 + 识别中可点「取消」
- 顶部状态文案上移（y 96 → 54），不再和对话区第一行重叠

**2026-11-19（深夜）**
- 中文字库改为**全局共享**：移到 `storage:/font-cjk.ttf`，由
  `components/cjk_font` 读入 PSRAM（selector 与所有 app 共用一份），
  app 不再内嵌 2 MB 字库，固件 4.0 MB → 2.1 MB
- `CONFIG_SPIFFS_PAGE_SIZE` 256 → 512：21 MB 分区用 256 会 mount 失败
  （`ESP_ERR_INVALID_ARG`）
- 镜像库升到 `app-xiaozhi-0.1.2.bin`

**2026-11-19（晚）**
- 中文改用 tiny_ttf 运行时渲染：`main/assets/xz_cjk.ttf`（GB2312 全集 7542 字，
  由 `scripts/gen_cjk_font.py` 从系统黑体裁剪），字号 18/26 px
- LVGL 从 64 KB 内置内存池切到系统堆（`LV_USE_CLIB_MALLOC`），
  tiny_ttf 字形缓存放得下
- 固件涨到 4.0 MB，超出 factory 2 MB → 新增 `flash_dev.ps1`，
  改为写 ota_0(0x220000) + `otatool switch_ota_partition`
- 显示缓冲从 PSRAM 改到内部 RAM（`buffer_height=32`），修掉刷屏
  `Draw bitmap failed: ESP_ERR_NO_MEM` 留下的永久残影
- 误用 `idf.py flash` 覆盖了 factory 里的 selector，已恢复：
  storage 换成 `app-xiaozhi-0.1.1.bin`（4.19 MB）、selector 写回 factory、擦 otadata

**2026-11-19**
- 打通完整对话链路：`listen start(manual)` → 说话 → `listen stop` → stt → tts 播报
- 修内存耗尽：音频任务栈搬到 PSRAM，PCM 缓冲显式放内部 RAM，
  `SPIRAM_MALLOC_RESERVE_INTERNAL` 32 KB → 64 KB，解决 LVGL 刷屏/tls 分配失败
- 修 `xz_play` 栈溢出（12 KB → 32 KB）
- 接入中文（思源黑体 16 px），界面提示文案汉化
- 按钮文案/颜色/可用性随会话状态变化；新增 `xz_chat_is_speaking()`
- 串口抓包脚本改 UTF-8
