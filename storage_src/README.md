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
- **一个 id 只能有一份 `.bin`**（`pack_storage.ps1` 遇到同 id 多版本会直接报错），
  换版本时把旧的挪进 `_archive/`

## 当前镜像库（2026-11-20）

| 文件 | 大小 | 说明 |
| --- | --- | --- |
| `app-xiaozhi-0.1.4.bin` | 2,113,520 B | 小智语音助手；音量改由 selector 设置页统一管理 |
| `app-gmeter-1.0.0.bin` | 745,312 B | 骑行加速度计 |
| `app-watch-0.1.5.bin` | 1,417,504 B | 怀表：指针表盘（刻度数字 + 日期窗）+ 秒表；watch 0.1.5 = sha256 `3423b8e6e2da…` |
| `font-cjk.ttf` | 2,111,252 B | CJK 共享字库（selector 与所有 app 共用） |

`manifest.json` 由脚本生成，不进仓库；最近一次打包：`partition_table=1a71e702`，
xiaozhi 0.1.4 = sha256 `f5d044f3ddf6…`。

## `_archive/`

`pack_storage.ps1` 只扫顶层 `app-*.bin`，不会递归子目录，所以旧的发布件放
`_archive/` 里既不会被打进镜像，也不会触发「重复 id」检查：

```
_archive/app-xiaozhi-0.1.2.bin
_archive/app-xiaozhi-0.1.3.bin
```

回滚时把归档文件拷回顶层、删掉同 id 的新版本，再跑一次 `tools/pack_storage.ps1 -Flash`。

## 容量规划

21MB 存放的是"可选 app 的完整二进制"，不是资源素材：

| 类型 | 典型大小 | 21MB 能放 |
| --- | --- | --- |
| 原生 LVGL app（如 ride-gmeter） | 0.7~1.5MB | 15~25 个 |
| 带解码/算法库的 app（音频、波形） | 3~6MB | 4~6 个 |
| Brookesia 规模应用 | 4~8MB | 2~4 个 |

素材（音乐、波形数据、图片包）不该塞进 app 二进制：它们属于数据，后续走单独的资源分区或外置存储，否则每次换 app 都要重刷一遍几百 MB 的东西。
