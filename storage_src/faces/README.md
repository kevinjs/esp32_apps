# faces —— 设备统一的拟人形象（表情包）

selector 与所有 app 共用的卡通形象。**素材不进任何 app 镜像**：`tools/pack_storage.ps1`
把这里的 PNG 转成 LVGL 原生二进制，写进 `storage` 分区（`0xA20000`，21MB），
固件侧由 `components/face_avatar` 读进 PSRAM 直接当 `lv_image_dsc_t` 用。

换表情只需要重打 storage（约 70 s），**不用重编/重发任何 app**。

## 文件清单

| 文件 | 用途 |
| --- | --- |
| `face-wait.png` | 启动 / 联网 / 连接服务器 |
| `face-idle.png` | 待命（**必需**，缺它整个表情包判定为不可用） |
| `face-blink.png` | 待命眨眼帧，app 定时插播 |
| `face-listen.png` | 聆听中 |
| `face-think.png` | 识别 / 思考中 |
| `face-talk-1.png` ~ `face-talk-3.png` | 播报口型循环，按序号连播（最多 8 帧） |
| `face-sad.png` | 出错 / 断线 / 没听清 |
| `face-happy.png` | 一轮完成（可选，未接的 app 忽略） |

## 素材规格

| 项 | 要求 |
| --- | --- |
| 格式 | PNG，32 位带 Alpha（`Format32bppArgb`），**不要 JPG** |
| 尺寸 | 正方形，≥ 384×384 即可（当前素材 363~397）；打包统一成 192×192 |
| 构图 | 主体居中、占画布约 80%，各帧的眼睛/脸位置要对齐 |
| 背景 | 全透明；圆屏外用纯黑，需要底色用 `#1B1B29` |
| 描边 | 源图 384 下线条 ≥ 6 px，否则缩到 192 会糊 |

打包时脚本会按不透明像素的包围盒自动裁剪再等比缩放居中，所以素材四周留白多寡
不影响结果；但**各帧的构图仍要自己对齐**，否则播报换帧时脸会左右跳。

## 产物与开销

```
face-<name>.bin  =  12 字节 lv_image_header + 192×192 ARGB8888
                  = 147,468 B/张（10 张共 1,474,680 B）
```

- flash：占 `storage` 约 1.4 MB（21 MB 分区的 7%）
- PSRAM：运行时整块读入，10 张约 1.4 MB
- 文件名 `face-<name>.bin` 都远低于 SPIFFS 的 31 字符上限

## 打包与验证

```powershell
cd d:\code\esp32_learning\esp32_apps
.\tools\pack_storage.ps1                 # 只生成 build\storage.bin
.\tools\pack_storage.ps1 -Flash -Port COM3   # 直接写 0xA20000
```

脚本日志会打印每张的表情尺寸和 sha256；`manifest.json` 里的 `faces` 数组记录
本次打进去的全部表情，便于排查「素材漏打」的问题。

## 固件侧用法

```c
#include "face_avatar.h"

if (face_avatar_init() == ESP_OK) {              // 缺 face-idle.bin 会返回 ESP_ERR_NOT_FOUND
    lv_obj_t *img = lv_image_create(lv_screen_active());
    lv_image_set_src(img, face_avatar_get(FACE_AVATAR_IDLE));   // 首次访问时读 PSRAM
    // 播报：face_avatar_talk_frames() / face_avatar_talk_frame(i)
}
```

- `face_avatar_size()` 返回边长（192），可直接用于布局
- 素材缺失时请回退到自绘界面，别让界面开天窗
- 不要在 LVGL 之外的线程里调用（内部会 fopen/fread，没有加锁）

## 参考实现

`xiaozhi/main/xz_ui.c`：图片表情 + 播报三帧轮播（200 ms/帧）+ 待命随机眨眼
（3~6 s）+ 透明度呼吸（`LV_OPA_80↔LV_OPA_COVER`），读不到素材时回退到几何简笔脸。

**不要对表情图用 `transform_scale` 做呼吸**：LVGL 的软件变换会给 192² 的图申请
约 74 KB 中间缓冲，且落在内部 RAM（实测把 `dma_max` 从 40 KB 压到 31 KB，
低于刷屏所需）。`LV_OPA` 在目标缓冲上逐像素混合，不额外占内存。

这套素材同时用于 selector 的磁贴图标：`face-idle.png` 经
`tools/make_app_icon.ps1 -NoCard` 生成 `storage_src/icon-xiaozhi@125.png`
（圆角裁切、无卡片底），所以应用列表里看到的也是同一个形象。
