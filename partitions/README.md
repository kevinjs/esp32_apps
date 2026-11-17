# 统一分区表

`fleet.csv` 是整块设备的唯一分区表，所有应用（selector、各功能 app）必须使用同一份，否则镜像无法互相切换。

## 布局

| 分区 | 偏移 | 大小 | 说明 |
| --- | --- | --- | --- |
| `nvs` | `0x9000` | 24KB | 标定零偏、配网信息。**偏移与大小不可改**，改了会丢数据 |
| `phy_init` | `0xF000` | 4KB | 射频校准数据 |
| `otadata` | `0x10000` | 8KB | 启动槽选择（`esp_ota_set_boot_partition` 写这里） |
| `factory` | `0x20000` | 3MB | selector（启动选择器），常驻，不被 app 覆盖 |
| `ota_0` | `0x320000` | 4MB | 运行槽，任一 app 镜像都写这里 |
| `storage` | `0x720000` | 20MB | LittleFS 镜像库（manifest + app bin + 图标） |

剩余约 4.9MB 未分配。

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
- `storage` 的子类型是 `littlefs`。如果 IDF 版本不认这个子类型（构建报 `invalid subtype`），把它改成 `spiffs` 并让挂载代码按分区标签 `storage` 查找即可。
