<#
.SYNOPSIS
    把镜像目录打包成 LittleFS 镜像（storage 分区），可选直接烧录。

.DESCRIPTION
    storage 分区（0x720000，20MB）存放镜像库：
        <SourceDir>/
        ├── app-<id>-<version>.bin
        ├── icons/<id>.png            （可选）
        └── manifest.json             （本脚本生成）

    脚本会扫描 *.bin、计算 sha256、生成 manifest.json，然后调用 mklittlefs
    生成 storage.bin。加 -Flash 时再用 esptool 写到 0x720000。

    只写 storage 分区，不会动 nvs（标定/配网）和 ota_0（当前 app）。

.PARAMETER SourceDir
    镜像源目录，默认 <repo>/storage_src

.PARAMETER OutDir
    输出目录，默认 <repo>/build

.PARAMETER Size
    storage 分区大小，默认 0x1400000 (20MB)，必须与 partitions/fleet.csv 一致

.PARAMETER Mklittlefs
    mklittlefs 可执行文件路径，默认从 PATH 查找。
    未安装时可用 pip 安装 littlefs-python 之外的独立工具，或从
    https://github.com/earlephilhower/mklittlefs/releases 下载。

.EXAMPLE
    .\pack_storage.ps1

.EXAMPLE
    .\pack_storage.ps1 -Flash -Port COM3
#>
param(
    [string]$SourceDir = (Join-Path $PSScriptRoot "..\storage_src"),
    [string]$OutDir = (Join-Path $PSScriptRoot "..\build"),
    [long]$Size = 0x1400000,
    [string]$Mklittlefs = "mklittlefs",
    [switch]$Flash,
    [string]$Port = "COM3",
    [int]$Baud = 921600
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$partitionsCsv = Join-Path $repoRoot "partitions\fleet.csv"

if (-not (Test-Path -LiteralPath $SourceDir)) {
    New-Item -ItemType Directory -Path $SourceDir -Force | Out-Null
    Write-Host "已创建空镜像目录: $SourceDir"
}
$SourceDir = (Resolve-Path -LiteralPath $SourceDir).Path

if (-not (Test-Path -LiteralPath $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}

# 分区表指纹：任何 app 镜像都要和 selector 用同一张表，selector 会用这个值校验
$partTableTag = "unknown"
if (Test-Path -LiteralPath $partitionsCsv) {
    $hash = (Get-FileHash -LiteralPath $partitionsCsv -Algorithm SHA256).Hash.ToLower()
    $partTableTag = $hash.Substring(0, 8)
}
else {
    Write-Warning "找不到 $partitionsCsv，manifest 的 partition_table 记为 unknown"
}

$entries = @()
$bins = Get-ChildItem -LiteralPath $SourceDir -Filter "*.bin" -File | Sort-Object Name
foreach ($bin in $bins) {
    if ($bin.Name -eq "storage.bin") { continue }

    $id = $bin.BaseName
    $version = "0.0.0"
    if ($bin.BaseName -match '^app-(?<id>.+)-(?<ver>\d+(\.\d+)*)$') {
        $id = $Matches['id']
        $version = $Matches['ver']
    }

    $hash = (Get-FileHash -LiteralPath $bin.FullName -Algorithm SHA256).Hash.ToLower()
    $icon = ""
    $iconPath = Join-Path (Join-Path $SourceDir "icons") "$id.png"
    if (Test-Path -LiteralPath $iconPath) {
        $icon = "icons/$id.png"
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

$manifest = [pscustomobject]@{
    version         = 1
    partition_table = $partTableTag
    generated       = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    apps            = @($entries)
}

$manifestPath = Join-Path $SourceDir "manifest.json"
$json = $manifest | ConvertTo-Json -Depth 6
[System.IO.File]::WriteAllText($manifestPath, $json, (New-Object System.Text.UTF8Encoding($false)))

Write-Host ("manifest.json: {0} 个 app, partition_table={1}" -f $entries.Count, $partTableTag)
foreach ($entry in $entries) {
    Write-Host ("  {0,-16} {1,-10} {2,10:N0} B  {3}" -f $entry.id, $entry.version, $entry.size, $entry.sha256.Substring(0, 12))
}

# ---- 打包 -------------------------------------------------------------------
$storageBin = Join-Path $OutDir "storage.bin"
$mklittlefsCmd = Get-Command $Mklittlefs -ErrorAction SilentlyContinue
if ($null -eq $mklittlefsCmd) {
    throw "找不到 mklittlefs。请安装后重试，或用 -Mklittlefs 指定路径。"
}

Write-Host ("mklittlefs -c {0} -b 4096 -p 256 -s {1} {2}" -f $SourceDir, $Size, $storageBin)
& $mklittlefsCmd.Source -c $SourceDir -b 4096 -p 256 -s $Size $storageBin
if ($LASTEXITCODE -ne 0) {
    throw "mklittlefs 失败（退出码 $LASTEXITCODE）"
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
    "write-flash", "0x720000", $storageBin
)
Write-Host ("python -m esptool {0}" -f ($esptoolArgs -join " "))
& python -m esptool @esptoolArgs
if ($LASTEXITCODE -ne 0) {
    throw "esptool 失败（退出码 $LASTEXITCODE）"
}
Write-Host "storage 分区已更新，nvs 与 ota_0 未受影响。"
