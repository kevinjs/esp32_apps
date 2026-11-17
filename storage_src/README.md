# storage_src

镜像库源目录，由 `tools/pack_storage.ps1` 打包成 LittleFS 镜像写入 `storage` 分区（`0x720000`，20MB）。

## 文件命名

```
app-<id>-<version>.bin      # 例：app-gmeter-1.0.0.bin
icons/<id>.png              # 可选，selector 列表图标
manifest.json               # 脚本自动生成，不要手改
```

`id` 用小写加连字符，`version` 用 `x.y.z`。文件名不匹配这个格式时，脚本会把整个文件名当 `id`、版本记为 `0.0.0`。

## 注意

- 这里放的是**已构建的 app 镜像**（`build/<project>.bin`），不是整段合并镜像。
- 所有镜像必须用 `partitions/fleet.csv` 构建，否则 selector 会因 `partition_table` 指纹不匹配拒绝写入。
- 20MB 大约能放 20 个 0.8MB 的原生 app，或 4 个左右 Brookesia 规模的应用。
