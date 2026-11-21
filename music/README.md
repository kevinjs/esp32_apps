# music（48 kHz WAV 音乐播放器）

一个独立的原生 ESP-IDF app：从 `storage` 分区读 `music-*.wav`，用
`esp_audio_simple_dec` 解 WAV，把 PCM 直接写进 ES8311 出声，界面上显示曲目、
格式、进度和播放状态。与 `watch` / `ride-gmeter` / `xiaozhi` 平级，
由 `selector` 装进 `ota_0` 启动，长按 BOOT 3 秒回到选择界面。

当前发布版：**0.1.0**（`storage_src/app-music-0.1.0.bin`，918,496 B，
sha256 前缀 `dd2a860196ca`）。版本号只体现在镜像库文件名里，代码里没有版本宏。

## 结构

```
music/
├─ CMakeLists.txt          # 挂 ../components（boot_switch / cjk_font / ui_bg / app_settings）
├─ partitions.csv          # fleet.csv 副本，方案 A 不改分区表
├─ sdkconfig.defaults
├─ flash_dev.ps1           # 调试用：写 ota_0 + 切启动分区（正式发布走镜像库）
├─ tools/
│  └─ gen_test_assets.py   # 生成 48 kHz 测试 WAV + selector 磁贴图标
└─ main/
   ├─ idf_component.yml    # BSP + esp_audio_codec
   ├─ music_config.h       # 采样率 / 缓冲区 / 颜色 / BOOT GPIO
   ├─ main.c               # NVS + boot_switch + 界面 + 播放器
   ├─ music_player.c       # storage 扫描、WAV 头解析、简易解码器播放任务
   └─ music_ui.c           # 渐变背景 + 曲目/格式/状态 + 进度条 + 播放按钮
```

## 音频通路

```
storage:/music-*.wav
   └─ fread 8 KB ─► esp_audio_simple_dec_process（WAV 容器解析 + PCM 解码）
                       └─ PCM（内部 RAM）─► esp_codec_dev_write ─► I2S ─► ES8311
```

| 项 | 值 |
| --- | --- |
| 采样率 | 48000 Hz |
| 声道 / 位宽 | 单声道 / 16 bit |
| I2S | 标准模式，MCLK 12.288 MHz（256 × 48 kHz），与 xiaozhi 同一套 BSP 引脚 |
| 解码输出缓冲 | 16 KB，`MALLOC_CAP_INTERNAL`（I2S DMA 读不到 PSRAM） |
| 读文件缓冲 | 8 KB，优先 PSRAM |
| 播放任务栈 | 8 KB（内部 RAM） |
| 音量 | 读 `components/app_settings`（selector 设置页写的 NVS，各 app 共享） |
| 循环 | 播到文件尾 → `fseek(0)` + **关掉重开解码器**（见下方踩坑） |

**0.1.0 只支持 48 kHz / 16 bit / 单声道 WAV**：I2S 与 ES8311 在启动时按这个
参数固定配置，文件名里的格式不符合时界面直接显示「仅支持 48000 Hz/16 bit/单声道」，
不会放出变调的声音。

### 两个必须知道的坑

1. **解码器不会自动注册**：`esp_audio_simple_dec_open()` 之前必须先调
   `esp_audio_dec_register_default()` + `esp_audio_simple_dec_register_default()`，
   否则 `esp_audio_simple_check_audio_type(WAV)` 直接返回 `NOT_SUPPORT`
   （日志 `WAV simple decoder not registered`）。
2. **循环播放不能用 `esp_audio_simple_dec_reset()`**：WAV 容器解析器被 reset 后
   不会回头重新解析 RIFF/WAVE 头，喂进去的数据全被吃掉——表现是 `pos` 一直 0、
   `peak` 0、文件瞬间读完、日志刷屏 `track end, loop`，最后把任务看门狗喂爆重启。
   现在每次循环都 `close` + `open`，只在换曲时发生，代价可忽略。
3. 解码器要按需裁剪：`sdkconfig.defaults` 里把非 WAV 的 `CONFIG_AUDIO_*_SUPPORT`
   全关掉。默认全开时整个 codec 库都链进镜像，实测 0.87 MB → 1.42 MB。

## 素材

音频是**数据**，不进 app 镜像（见 `storage_src/README.md` 的容量规划）：

```powershell
cd d:\code\esp32_learning\esp32_apps\music
python tools\gen_test_assets.py      # 生成两个文件到 ../storage_src/
#   music-test48k.wav   48 kHz / 16 bit / mono / 8 s，C 大调五声音阶琶音 + C3 低音
#   icon-music.png      256x256 透明底白色连音符（selector 磁贴用）
```

放新歌：命名成 `music-<名字>.wav`（文件名 ≤31 字符）丢进 `storage_src/`，
重跑 `..\tools\pack_storage.ps1 -Flash` 即可，**不用重编 app**。
app 启动时按文件名排序取第一首，播放中在曲目行显示文件名。

## 构建与发布

```powershell
cd d:\code\esp32_learning\esp32_apps\music
. $env:IDF_PATH\export.ps1           # 或已导出的 IDF 终端
idf.py build                          # 产物 build/music.bin（918,496 B）

# 调试：直接写 ota_0 并切启动分区（不经过 selector）
.\flash_dev.ps1 -Port COM3

# 正式流程（进镜像库，selector 列表里出现「音乐」）：
Copy-Item build\music.bin ..\storage_src\app-music-0.1.0.bin -Force
..\tools\pack_storage.ps1 -Flash      # 21 MB 镜像库 + 测试 WAV，约 70s
# 从 selector 里点「音乐」启动
```

> **不要用 `idf.py flash`**：它固定写 `0x20000`（factory），会覆盖 selector。
> 恢复：`..\tools\flash_app.ps1 -Bin ..\selector\build\selector.bin -Slot selector`，
> 再擦 otadata 让 bootloader 回落 factory。

上机验证（串口日志）：

| 日志 | 含义 |
| --- | --- |
| `music: wav: 48000 Hz / 1 ch / 16 bit, 8000 ms, data=768000 B @44` | WAV 头解析结果 |
| `music.player: speaker ready: 48000 Hz / 1 ch / 16 bit, volume 60%` | ES8311 打开成功 |
| `music.player: pos 2000/8000 ms, peak 8123, int=... dma=...` | 每 2 s 一行：播放位置、PCM 峰值（静音 0，正常 3000~20000）、内存余量 |
| `music.player: track end, loop` | 播完一轮，回到文件头 |
| `music.player: decode failed: N` | 解码失败，界面转「播放失败」 |
| `eof` 卡住 / `peak 0` | I2S/ES8311 没出声，先看 `speaker open failed` 与音量 |

0.1.0 在 COM3 上实测（音量 100%）：

```
I (4148) music.player: [heap] start  internal=261591 dma=253803 dma_max=139264 psram=4841428
I (4221) music.player: pos 80/8000 ms, peak 17227
I (6271) music.player: pos 2128/8000 ms, peak 18020
I (12141) music.player: track end, loop        # 8 s 一轮，循环无 crackle、无重启
I (20416) music.player: track end, loop        # 连续 5 轮，内存曲线持平
```

PCM 峰值 17000~18000（满量程 32767）说明数据完整送到 I2S；内部 RAM 261 KB、
DMA 最大连续块 139 KB，离刷屏需要的 46 KB 有充足余量。

## 界面

- 背景：共享组件 `components/ui_bg`，与 selector 相同的 30° 斜向渐变（PSRAM，434 KB）
- 中央圆形"唱片"：`LV_SYMBOL_AUDIO`，不依赖图片素材
- 曲目行：文件名；下面一行是 `48000 Hz / 16 bit / 单声道`（出错时显示原因）
- 进度条 + 左右 `m:ss` 时间
- 底部圆形按钮：播放 / 暂停（没有曲目时变暗且不可点）
- 中文用共享字库 `components/cjk_font`（`storage:/font-cjk.ttf`），缺失时回退 Montserrat

## 变更记录

**2026-11-21（0.1.0）**
- 新建 app：storage 扫描 `music-*.wav`，`esp_audio_simple_dec` 解 WAV，
  48 kHz PCM 写 ES8311，播完自动循环
- 界面：渐变背景 + 曲目/格式/状态 + 进度条 + 播放/暂停按钮
- 新增 `tools/gen_test_assets.py`（48 kHz 8 s 测试音频 + 音符图标），
  `tools/pack_storage.ps1` 支持把 `music-*.wav` 打进镜像库
- selector 磁贴新增「音乐」（`APP_LOOKS` 里 id = `music`）
- 固件 918,496 B，发布件 `storage_src/app-music-0.1.0.bin`
  （sha256 `dd2a860196ca…`）；上机验证连续循环 5 轮无重启、峰值 17k~18k

## 已知限制

- 只认 `music-*.wav` 的第一首（按文件名排序），没有播放列表/切歌
- 只支持 48 kHz / 16 bit / 单声道；44.1 kHz 或立体声需要改 I2S 配置重编
- 暂停时 I2S 空转（不出声但时钟还在），没有做真正常停时钟的省电逻辑
- 没有拖动进度/音量调节：音量统一在 selector 设置页改
- 时间显示按文件长度估算（WAV 头里的 data 大小），不是精确到采样点
