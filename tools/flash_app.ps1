<#
.SYNOPSIS
    按分区表烧录 app / 布局（不要用 idf.py flash）。

.DESCRIPTION
    idf.py flash 会把 app 写到 factory(0x20000)，也就是覆盖 selector。
    本脚本按固定偏移写入，只动目标分区，nvs（标定/配网）不受影响。

    分区偏移来自 partitions/fleet.csv：
        bootloader         0x0        首次烧录 / 换分区表时
        分区表             0x8000     首次烧录 / 换分区表时
        factory(selector)  0x20000    2MB
        ota_0(运行槽)      0x220000   8MB
        storage(镜像库)    0xA20000   21MB  ← 由 pack_storage.ps1 写入

    -Slot layout 会从 -Bin 所在构建目录取 bootloader.bin 与 partition-table.bin，
    写入 0x0 和 0x8000。只换 app 时不涉及这两个分区。

.PARAMETER Bin
    app 镜像，通常是 build/<project>.bin

.PARAMETER Slot
    "0" = ota_0（运行槽）；"selector" = factory；"layout" = bootloader + 分区表。

.EXAMPLE
    # 首次烧录：先写布局，再写 selector
    .\flash_app.ps1 -Bin ..\selector\build\selector.bin -Slot layout -Port COM3
    .\flash_app.ps1 -Bin ..\selector\build\selector.bin -Slot selector -Port COM3

.EXAMPLE
    .\flash_app.ps1 -Bin ..\ride-gmeter\build\ride-gmeter.bin -Slot 0 -Port COM3
#>
param(
    [Parameter(Mandatory = $true)][string]$Bin,
    [Parameter(Mandatory = $true)][ValidateSet("0", "selector", "layout")][string]$Slot,
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

if ($Slot -eq "layout") {
    $buildDir = Split-Path -Parent (Resolve-Path -LiteralPath $Bin).Path
    $bootBin = Join-Path $buildDir "bootloader\bootloader.bin"
    $tableBin = Join-Path $buildDir "partition_table\partition-table.bin"
    foreach ($path in @($bootBin, $tableBin)) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "找不到 $path，请先完整构建一次（idf.py build）"
        }
    }
    Write-Host "写入 bootloader -> 0x0, 分区表 -> 0x8000"
    $esptoolArgs += @("write-flash", "0x0", $bootBin, "0x8000", $tableBin)
}
else {
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

    # 不传 --flash-mode/--flash-freq/--flash-size：app 镜像头里已经带着构建时的参数，
    # 覆盖它反而可能导致启动失败。
    $esptoolArgs += @("write-flash", $offset, $Bin)
}

Write-Host ("python -m esptool {0}" -f ($esptoolArgs -join " "))

# esptool 退出后 Windows 偶尔要几秒才重新枚举 USB CDC，这里重试几次
$attempt = 0
while ($true) {
    $attempt++
    $ErrorActionPreference = "Continue"
    & python -m esptool @esptoolArgs
    $code = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    if ($code -eq 0) {
        break
    }
    if ($attempt -ge 3) {
        throw "esptool 失败（退出码 $code）。若提示端口占用，请拔插 USB 或按板上 RESET 后重试。"
    }
    Write-Warning "esptool 失败（退出码 $code），3 秒后重试 $attempt/3 ..."
    Start-Sleep -Seconds 3
}

Write-Host "完成。"
