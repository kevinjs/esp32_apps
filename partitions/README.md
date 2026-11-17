# 统一分区表

`fleet.csv` 是整块设备的唯一分区表，所有应用（selector、各功能 app）必须使用同一份，否则镜像无法互相切换。

## 布局（32MB Flash）

| 分区 | 偏移 | 大小 | 说明 |
| --- | --- | --- | --- |
| `nvs` | `0x9000` | 24KB | 标定零偏、配网信息。**偏移与大小不可改**，改了会丢数据 |
| `phy_init` | `0xF000` | 4KB | 射频校准数据 |
| `otadata` | `0x10000` | 8KB | 启动槽选择（`esp_ota_set_boot_partition` 写这里） |
| `factory` | `0x20000` | 2MB | selector（启动选择器），常驻，不被 app 覆盖 |
| `ota_0` | `0x220000` | 8MB | 运行槽，任一 app 镜像都写这里 |
| `storage` | `0xA20000` | 21MB | SPIFFS 镜像库（manifest + app bin + 图标） |

剩余约 0.9MB 未分配。

运行槽给到 8MB 是为了容纳尺寸不定的 app（音频解码、FFT、波形渲染可能到 3~6MB）；镜像库是"任意数量、任意大小"，不受槽位数量限制。

## 使用方式

在每个应用的工程根目录放一份同名副本：

```powershell
copy ..\partitions\fleet.csv .\partitions.csv
```

对应 `sdkconfig.defaults`：

```
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
```

## 常见错误

- **不要用 `idf.py flash` 烧应用。** 表里有 `factory`，IDF 会把 app 烧到 `0x20000`，直接覆盖 selector。开发时用 `tools/flash_app.ps1 -Slot 0`。
- **不要改动 `nvs` 的偏移或大小。** 一旦改变，设备会重新初始化 NVS，标定和配网全部丢失。
- **`storage` 是 SPIFFS**，打包参数必须和 selector 的 `sdkconfig` 一致（page 256 / block 4096 / obj-name 32 / meta 4），统一由 `tools/pack_storage.ps1` 处理，不要手写 `spiffsgen.py` 命令。
- 改了这张表，`manifest.json` 里的 `partition_table` 指纹会变，镜像库需要重新打包。
