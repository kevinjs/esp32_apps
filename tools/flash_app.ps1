<#
.SYNOPSIS
    按分区表把 app 写到指定槽位（不要用 idf.py flash）。

.DESCRIPTION
    idf.py flash 会把 app 写到 factory(0x20000)，也就是覆盖 selector。
    本脚本按固定偏移写入，只动目标分区，nvs（标定/配网）不受影响。

    分区偏移来自 partitions/fleet.csv：
        factory(selector)  0x20000    2MB
        ota_0(运行槽)      0x220000   8MB   ← 可容纳尺寸较大的 app
        storage(镜像库)    0xA20000   21MB  ← 由 pack_storage.ps1 写入

.PARAMETER Bin
    app 镜像，通常是 build/<project>.bin

.PARAMETER Slot
    "0" 写到 ota_0（运行槽）；"selector" 写到 factory。

.EXAMPLE
    .\flash_app.ps1 -Bin ..\ride-gmeter\build\ride-gmeter.bin -Slot 0 -Port COM3

.EXAMPLE
    .\flash_app.ps1 -Bin ..\selector\build\selector.bin -Slot selector -Port COM3
#>
param(
    [Parameter(Mandatory = $true)][string]$Bin,
    [Parameter(Mandatory = $true)][ValidateSet("0", "selector")][string]$Slot,
    [string]$Port = "COM3",
    [int]$Baud = 921600,
    [switch]$Erase
)

$ErrorActionPreference = "Stop"

$offsets = @{
    "selector" = "0x20000"
    "0"        = "0x220000"
}

if (-not (Test-Path -LiteralPath $Bin)) {
    throw "镜像不存在: $Bin"
}

$offset = $offsets[$Slot]
$size = (Get-Item -LiteralPath $Bin).Length

# ota_0 是 8MB，超出会被 selector 拒绝启动，这里先挡一次
if ($Slot -eq "0" -and $size -gt 0x800000) {
    throw ("镜像 {0:N0} 字节超过 ota_0 分区 8MB，请先调整 partitions/fleet.csv" -f $size)
}
if ($Slot -eq "selector" -and $size -gt 0x200000) {
    throw ("镜像 {0:N0} 字节超过 factory 分区 2MB" -f $size)
}

Write-Host ("写入 {0} -> {1} ({2:N0} 字节)" -f (Split-Path $Bin -Leaf), $Slot, $size)

$esptoolArgs = @(
    "--chip", "esp32s3",
    "-p", $Port,
    "-b", $Baud,
    "--before", "default-reset",
    "--after", "hard-reset"
)
if ($Erase) {
    $esptoolArgs += @("erase-flash")
}

# 不传 --flash-mode/--flash-freq/--flash-size：app 镜像头里已经带着构建时的参数，
# 覆盖它反而可能导致启动失败。
$esptoolArgs += @("write-flash", $offset, $Bin)

Write-Host ("python -m esptool {0}" -f ($esptoolArgs -join " "))
& python -m esptool @esptoolArgs
if ($LASTEXITCODE -ne 0) {
    throw "esptool 失败（退出码 $LASTEXITCODE）"
}

Write-Host "完成。若目标槽当前未被启动，请在 selector 中选择该 app。"
