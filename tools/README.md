# 工具

多 app 镜像方案配套脚本。分区表见 [`../partitions/fleet.csv`](../partitions/fleet.csv)，方案说明见 [`../docs/multi-app-image-management.md`](../docs/multi-app-image-management.md)。

| 脚本 | 用途 |
| --- | --- |
| `flash_app.ps1` | 按槽位烧录 app（`ota_0` 或 `factory`/selector），不动 nvs |
| `pack_storage.ps1` | 收集镜像 → 生成 manifest → `spiffsgen.py` 打包 → 可选烧录 storage |

## 固定偏移

| 目标 | 偏移 | 大小 | 写入方式 |
| --- | --- | --- | --- |
| selector | `0x20000` | 2MB | `flash_app.ps1 -Slot selector` |
| 运行槽 app | `0x220000` | 8MB | `flash_app.ps1 -Slot 0` |
| 镜像库 | `0xA20000` | 21MB | `pack_storage.ps1 -Flash` |

> selector 还没写出来之前，应用先烧到 `-Slot selector`（otadata 为空时 bootloader 会启动 factory）。selector 落地后，应用改烧 `-Slot 0`，由 selector 选启动。

## 日常流程

```powershell
# 1. 构建 selector（首次）
cd ..\selector
idf.py build
..\tools\flash_app.ps1 -Bin build\selector.bin -Slot selector -Port COM3

# 2. 构建 app（不要 idf.py flash）
cd ..\ride-gmeter
idf.py build

# 3. 写到运行槽，直接启动
cd ..\tools
.\flash_app.ps1 -Bin ..\ride-gmeter\build\ride-gmeter.bin -Slot 0 -Port COM3

# 4. 更新镜像库：复制进 storage_src，打包并烧录
Copy-Item ..\ride-gmeter\build\ride-gmeter.bin ..\storage_src\app-gmeter-1.0.0.bin
.\pack_storage.ps1 -Flash -Port COM3
```

## 注意

- **永远不要用 `idf.py flash`**：分区表里有 `factory`，IDF 会把 app 烧到 `0x20000`，覆盖 selector。
- `pack_storage.ps1` 用 IDF 自带的 `spiffsgen.py`，不需要额外安装 `mklittlefs`；但必须在 ESP-IDF 环境里跑（需要 `IDF_PATH` 和 `python`）。
- SPIFFS 参数（page 256 / block 4096 / obj-name 32 / meta 4）必须和 selector 的 `sdkconfig` 一致，脚本里已固定，不要单独改。
- `manifest.json` 里的 `partition_table` 是 `partitions/fleet.csv` 的 SHA256 前 8 位；改了分区表要重新打包。
