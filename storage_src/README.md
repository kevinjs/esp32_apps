# storage_src

镜像库源目录，由 `tools/pack_storage.ps1` 打包成 SPIFFS 镜像写入 `storage` 分区（`0xA20000`，21MB）。

## 文件命名

```
app-<id>-<version>.bin      # 例：app-gmeter-1.0.0.bin
icon-<id>.png               # 可选，selector 列表图标
manifest.json               # 脚本自动生成，不要手改
```

- `id` 用小写加连字符，`version` 用 `x.y.z`
- **文件名最长 31 字符**（SPIFFS 对象名上限），超了脚本会直接报错
- 平铺存放，不接受子目录（SPIFFS 没有目录）
- 本 README 不会被打进镜像：脚本只收集 `app-*.bin` / `icon-*.png`

## 容量规划

21MB 存放的是"可选 app 的完整二进制"，不是资源素材：

| 类型 | 典型大小 | 21MB 能放 |
| --- | --- | --- |
| 原生 LVGL app（如 ride-gmeter） | 0.7~1.5MB | 15~25 个 |
| 带解码/算法库的 app（音频、波形） | 3~6MB | 4~6 个 |
| Brookesia 规模应用 | 4~8MB | 2~4 个 |

素材（音乐、波形数据、图片包）不该塞进 app 二进制：它们属于数据，后续走单独的资源分区或外置存储，否则每次换 app 都要重刷一遍几百 MB 的东西。
