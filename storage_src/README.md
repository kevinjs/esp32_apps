# storage_src

镜像库源目录，由 `tools/pack_storage.ps1` 打包成 SPIFFS 镜像写入 `storage` 分区（`0xA20000`，21MB）。

## 文件命名

```
app-<id>-<version>.bin      # 例：app-gmeter-1.0.0.bin
icon-<id>.png               # 可选，selector 磁贴图标 88x88（打包时转成 icon-<id>.bin，四周留边）
icon-<id>@<px>.png          # 可选，满幅图标（@132 = 铺满 132px 磁贴，源图自带圆角）
font-*.ttf                  # 共享字库（原样进镜像）
music-*.wav                 # 共享音频素材（原样进镜像，music app 直接从 storage 读）
faces/face-<name>.png       # 共享表情包源图（打包时转成 face-<name>.bin，源图不进镜像）
manifest.json               # 脚本自动生成，不要手改
```

- `id` 用小写加连字符，`version` 用 `x.y.z`
- **文件名最长 31 字符**（SPIFFS 对象名上限），超了脚本会直接报错
- 平铺存放，不接受子目录（SPIFFS 没有目录）；唯一例外是 `faces/` 素材目录，
  它只是打包时的输入，转换后的 `face-*.bin` 仍平铺在镜像根目录
- 本 README 不会被打进镜像：脚本只收集 `app-*.bin` / `icon-*.png` / `font-*.ttf` /
  `music-*.wav` / `faces/face-*.png`
- **一个 id 只能有一份 `.bin`**（`pack_storage.ps1` 遇到同 id 多版本会直接报错），
  换版本时把旧的挪进 `_archive/`

## 当前镜像库（2026-11-21）

| 文件 | 大小 | 说明 |
| --- | --- | --- |
| `app-xiaozhi-0.1.8.bin` | 2,117,824 B | 小智语音助手；共享表情包（192px 图片表情）+ selector 同款渐变背景 |
| `app-gmeter-1.0.0.bin` | 745,312 B | 骑行加速度计 |
| `app-watch-0.1.6.bin` | 1,418,000 B | 怀表：指针表盘（刻度数字 + 日期窗）+ 秒表；watch 0.1.6 = sha256 `5c44c91c8c81…` |
| `app-music-0.1.0.bin` | 918,496 B | 音乐播放器：WAV（48 kHz/16 bit/单声道）→ ES8311；music 0.1.0 = sha256 `dd2a860196ca…` |
| `icon-watch.png` / `icon-gmeter.png` | 1.6~4.1 KB | 图标源图（256×256 白色线条 + 透明底）；打包时转成 88×88 的 `icon-<id>.bin` 存进镜像 |
| `icon-music.png` | 2.8 KB | 音符图标源图（256×256 白色线条 + 透明底），由 `music/tools/gen_test_assets.py` 生成 |
| `icon-xiaozhi@125.png` | 141 KB | 小智图标：微笑脸圆角裁切、无卡片底无描边，125×125（磁贴 132 的 0.95，四周留 3.5 px）；磁贴不画文字标题；`tools/make_app_icon.ps1 -NoCard -RadiusRatio 0.24` 从 `faces/face-idle.png` 生成 |
| `font-cjk.ttf` | 2,111,252 B | CJK 共享字库（selector 与所有 app 共用） |
| `music-test48k.wav` | 768,044 B | 48 kHz / 16 bit / 单声道 / 8 s 测试音频（C 大调琶音），由 `music/tools/gen_test_assets.py` 生成；换歌就替换同名文件后重打 storage，不用重编 app |
| `faces/face-*.png` | 10 张，1.9 MB | 共享表情包源图（396² PNG + Alpha）；打包时转成 192×192 的 `face-<name>.bin`，10 张共 1,474,680 B |

`manifest.json` 由脚本生成，不进仓库；最近一次打包：`partition_table=1a71e702`，
xiaozhi 0.1.8 = sha256 `e76ca2e56305…`，music 0.1.0 = `dd2a860196ca…`。

## `_archive/`

`pack_storage.ps1` 只扫顶层 `app-*.bin`，不会递归子目录，所以旧的发布件放
`_archive/` 里既不会被打进镜像，也不会触发「重复 id」检查：

```
_archive/app-xiaozhi-0.1.3.bin
_archive/app-xiaozhi-0.1.4.bin
_archive/app-xiaozhi-0.1.5.bin
_archive/app-xiaozhi-0.1.6.bin      # 过渡版：图片表情 + 缩放呼吸（吃内部 RAM），被 0.1.7 取代
_archive/app-xiaozhi-0.1.7.bin
```

表情包没有归档概念：`faces/` 里的 PNG 是唯一来源，改素材后重跑打包即可覆盖
`face-*.bin`；想留旧版就把整份 `faces/` 目录拷到 `_archive/faces-<日期>/`。

回滚时把归档文件拷回顶层、删掉同 id 的新版本，再跑一次 `tools/pack_storage.ps1 -Flash`。

## 容量规划

21MB 存放的是"可选 app 的完整二进制"，不是资源素材：

| 类型 | 典型大小 | 21MB 能放 |
| --- | --- | --- |
| 原生 LVGL app（如 ride-gmeter） | 0.7~1.5MB | 15~25 个 |
| 带解码/算法库的 app（音频、波形） | 3~6MB | 4~6 个 |
| Brookesia 规模应用 | 4~8MB | 2~4 个 |

素材（音乐、波形数据、图片包）不该塞进 app 二进制：它们属于数据，后续走单独的资源分区或外置存储，否则每次换 app 都要重刷一遍几百 MB 的东西。
