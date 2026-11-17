<#
.SYNOPSIS
    把镜像目录打包成 SPIFFS 镜像（storage 分区），可选直接烧录。

.DESCRIPTION
    storage 分区（0xA20000，21MB）存放镜像库，平铺结构（SPIFFS 不支持子目录）：
        <SourceDir>/
        ├── app-<id>-<version>.bin     文件名单个不超过 31 字符（CONFIG_SPIFFS_OBJ_NAME_LEN=32）
        ├── icon-<id>.png              （可选）
        └── manifest.json              （本脚本生成）

    脚本会：
      1. 收集 app-*.bin / icon-*.png，校验文件名长度
      2. 计算 sha256、生成 manifest.json
      3. 复制到临时目录（避免把 README 之类的东西打进镜像）
      4. 调用 IDF 自带 spiffsgen.py 打包
      5. 加 -Flash 时用 esptool 写到 0xA20000

    只写 storage 分区，不会动 nvs（标定/配网）和 ota_0（当前 app）。

.PARAMETER SourceDir
    镜像源目录，默认 <repo>/storage_src

.PARAMETER OutDir
    输出目录，默认 <repo>/build

.PARAMETER Size
    storage 分区大小，默认 0x1500000 (21MB)，必须与 partitions/fleet.csv 一致

.PARAMETER IdfPath
    ESP-IDF 路径，默认取 $env:IDF_PATH（在 ESP-IDF PowerShell 里已设置）

.EXAMPLE
    .\pack_storage.ps1

.EXAMPLE
    .\pack_storage.ps1 -Flash -Port COM3
#>
param(
    [string]$SourceDir = (Join-Path $PSScriptRoot "..\storage_src"),
    [string]$OutDir = (Join-Path $PSScriptRoot "..\build"),
    [long]$Size = 0x1500000,
    [string]$IdfPath = $env:IDF_PATH,
    [switch]$Flash,
    [string]$Port = "COM3",
    [int]$Baud = 921600
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$partitionsCsv = Join-Path $repoRoot "partitions\fleet.csv"

if ([string]::IsNullOrWhiteSpace($IdfPath)) {
    throw "未设置 IDF_PATH。请在 ESP-IDF PowerShell 中运行，或用 -IdfPath 指定。"
}
$spiffsgen = Join-Path $IdfPath "components\spiffs\spiffsgen.py"
if (-not (Test-Path -LiteralPath $spiffsgen)) {
    throw "找不到 spiffsgen.py: $spiffsgen"
}

if (-not (Test-Path -LiteralPath $SourceDir)) {
    New-Item -ItemType Directory -Path $SourceDir -Force | Out-Null
    Write-Host "已创建空镜像目录: $SourceDir"
}
$SourceDir = (Resolve-Path -LiteralPath $SourceDir).Path

if (-not (Test-Path -LiteralPath $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}

# 分区表指纹：镜像库要和 selector 用同一张表
$partTableTag = "unknown"
if (Test-Path -LiteralPath $partitionsCsv) {
    $hash = (Get-FileHash -LiteralPath $partitionsCsv -Algorithm SHA256).Hash.ToLower()
    $partTableTag = $hash.Substring(0, 8)
}
else {
    Write-Warning "找不到 $partitionsCsv，manifest 的 partition_table 记为 unknown"
}

# ---- 收集镜像 ---------------------------------------------------------------
$entries = @()
$bins = Get-ChildItem -LiteralPath $SourceDir -Filter "app-*.bin" -File | Sort-Object Name
foreach ($bin in $bins) {
    # SPIFFS 对象名最长 31 字符（CONFIG_SPIFFS_OBJ_NAME_LEN=32，含结尾 NUL）
    if ($bin.Name.Length -gt 31) {
        throw ("文件名过长（{0} 字符，上限 31）: {1}，请缩短 id 或版本号" -f $bin.Name.Length, $bin.Name)
    }

    $id = $bin.BaseName
    $version = "0.0.0"
    if ($bin.BaseName -match '^app-(?<id>.+)-(?<ver>\d+(\.\d+)*)$') {
        $id = $Matches['id']
        $version = $Matches['ver']
    }

    $hash = (Get-FileHash -LiteralPath $bin.FullName -Algorithm SHA256).Hash.ToLower()
    $icon = ""
    $iconPath = Join-Path $SourceDir "icon-$id.png"
    if (Test-Path -LiteralPath $iconPath) {
        $icon = "icon-$id.png"
    }

    $entries += [pscustomobject]@{
        id      = $id
        name    = $id
        version = $version
        bin     = $bin.Name
        size    = $bin.Length
        sha256  = $hash
        icon    = $icon
    }
}

if ($entries.Count -eq 0) {
    Write-Warning "目录里没有 app-*.bin，生成的镜像库是空的"
}

$duplicated = $entries | Group-Object id | Where-Object { $_.Count -gt 1 }
if ($duplicated) {
    throw ("存在重复 app id: " + (($duplicated | ForEach-Object { $_.Name }) -join ", "))
}

# ---- 暂存目录（只放要进镜像的文件） -----------------------------------------
$stage = Join-Path $env:TEMP "selector_storage_stage"
if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Path $stage -Force | Out-Null

foreach ($entry in $entries) {
    Copy-Item -LiteralPath (Join-Path $SourceDir $entry.bin) -Destination $stage
    if ($entry.icon -ne "") {
        Copy-Item -LiteralPath (Join-Path $SourceDir $entry.icon) -Destination $stage
    }
}

$manifest = [pscustomobject]@{
    version         = 1
    partition_table = $partTableTag
    generated       = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    apps            = @($entries)
}
$manifestPath = Join-Path $stage "manifest.json"
$json = $manifest | ConvertTo-Json -Depth 6
[System.IO.File]::WriteAllText($manifestPath, $json, (New-Object System.Text.UTF8Encoding($false)))

Write-Host ("manifest.json: {0} 个 app, partition_table={1}" -f $entries.Count, $partTableTag)
foreach ($entry in $entries) {
    Write-Host ("  {0,-16} {1,-10} {2,10:N0} B  {3}" -f $entry.id, $entry.version, $entry.size, $entry.sha256.Substring(0, 12))
}

# ---- 打包 -------------------------------------------------------------------
$storageBin = Join-Path $OutDir "storage.bin"
$spiffsArgs = @(
    $spiffsgen,
    "--page-size", "256",
    "--block-size", "4096",
    "--obj-name-len", "32",
    "--meta-len", "4",
    $Size, $stage, $storageBin
)
Write-Host ("python {0}" -f ($spiffsArgs -join " "))
& python @spiffsArgs
if ($LASTEXITCODE -ne 0) {
    throw "spiffsgen 失败（退出码 $LASTEXITCODE）"
}

$packedSize = (Get-Item -LiteralPath $storageBin).Length
Write-Host ("已生成 {0} ({1:N0} 字节，分区 {2:N0} 字节)" -f $storageBin, $packedSize, $Size)

if (-not $Flash) {
    return
}

# ---- 烧录 -------------------------------------------------------------------
$esptoolArgs = @(
    "--chip", "esp32s3",
    "-p", $Port,
    "-b", $Baud,
    "--before", "default-reset",
    "--after", "hard-reset",
    "write-flash", "0xA20000", $storageBin
)
Write-Host ("python -m esptool {0}" -f ($esptoolArgs -join " "))
& python -m esptool @esptoolArgs
if ($LASTEXITCODE -ne 0) {
    throw "esptool 失败（退出码 $LASTEXITCODE）"
}
Write-Host "storage 分区已更新，nvs 与 ota_0 未受影响。"
