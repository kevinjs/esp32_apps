<#
把 music 烧进 ota_0（8 MB）并切换启动分区。

为什么不用 idf.py flash：
    idf.py flash 固定写 0x20000（factory），那里是 selector，覆盖后长按 BOOT
    回不到选择器。正式流程是发布到镜像库（见 README），本脚本只用于调试。

用法（已 build 过）：
    .\flash_dev.ps1
    .\flash_dev.ps1 -Port COM5 -Baud 921600

只写应用分区，不动 bootloader / 分区表 / storage。
#>
param(
    [string]$Port = "COM3",
    [int]$Baud = 460800
)

$ErrorActionPreference = "Stop"
Push-Location $PSScriptRoot

$idf = $env:IDF_PATH
if (-not $idf) { $idf = "C:\esp\v6.1\esp-idf" }

$app = "build\music.bin"
if (-not (Test-Path $app)) {
    Pop-Location
    throw "找不到 $app，先执行 idf.py build"
}

$size = (Get-Item $app).Length
Write-Host ("app  {0:N2} MB -> 0x220000 (ota_0, 8 MB)" -f ($size / 1MB))

try {
    python -m esptool --chip esp32s3 -p $Port -b $Baud `
        --before default-reset --after hard-reset `
        write-flash 0x220000 $app

    # 让 bootloader 从 ota_0 启动（写 otadata）
    # otatool 依赖 parttool，需要把 IDF 的 python 模块目录塞进 PYTHONPATH
    $env:PYTHONPATH = "$idf\components\partition_table;$idf\components\app_update;$env:PYTHONPATH"
    python "$idf\components\app_update\otatool.py" -p $Port switch_ota_partition --name ota_0
}
finally {
    Pop-Location
}

Write-Host "完成：复位后应从 ota_0 启动。"
Write-Host ""
Write-Host "提醒：本脚本只是临时验证，ota_0 里的镜像不在 selector 镜像库里。" -ForegroundColor Yellow
Write-Host "      验证通过后记得发布（否则 selector 里启动的仍是旧版本）：" -ForegroundColor Yellow
Write-Host "        Copy-Item build\music.bin ..\storage_src\app-music-<版本>.bin -Force" -ForegroundColor Yellow
Write-Host "        ..\tools\pack_storage.ps1 -Flash" -ForegroundColor Yellow
