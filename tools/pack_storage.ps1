<#
.SYNOPSIS
    把镜像目录打包成 SPIFFS 镜像（storage 分区），可选直接烧录。

.DESCRIPTION
    storage 分区（0xA20000，21MB）存放镜像库，平铺结构（SPIFFS 不支持子目录）：
        <SourceDir>/
        ├── app-<id>-<version>.bin     文件名单个不超过 31 字符（CONFIG_SPIFFS_OBJ_NAME_LEN=32）
        ├── icon-<id>.png              （可选，源图；打包时转成 icon-<id>.bin，88x88 四周留边）
        ├── icon-<id>@<px>.png         （可选，满幅图标；如 @132 = 铺满 132px 磁贴）
        ├── font-*.ttf                 （共享字库，原样进镜像）
        ├── music-*.wav                （共享音频素材，原样进镜像；app 从 storage 读）
        ├── faces/face-<name>.png      （表情包源图，打包时转成 face-<name>.bin，不进镜像）
        └── manifest.json              （本脚本生成）

    脚本会：
      1. 收集 app-*.bin / icon-*.png / font-*.ttf / music-*.wav / faces/face-*.png，校验文件名长度
      2. 计算 sha256、把 PNG 图标转成 LVGL 二进制（88x88 ARGB8888，约 31 KB）
      3. 把 faces/ 下的表情素材转成 face-<name>.bin（192x192 ARGB8888，约 148 KB/张）
      4. 生成 manifest.json，复制到临时目录（避免把 README 之类打进镜像）
      5. 调用 IDF 自带 spiffsgen.py 打包
      6. 加 -Flash 时用 esptool 写到 0xA20000

    图标/表情为什么要转：固件里不开 PNG 解码器（libpng/lodepng 会占上百 KB flash，
    解码还要占 LVGL 的小堆），转成 LVGL 原生二进制后运行时只需 read 进 PSRAM，
    直接当 lv_image_dsc_t 用。固件侧统一由 components/face_avatar 读取。

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

# ---- 图标源：icon-<id>.png（默认 88，四周留边）或 icon-<id>@<px>.png（满幅） --
# 磁贴是 132x132，图标按图片自带尺寸 1:1 显示：
#   icon-<id>.png       → icon-<id>.bin  88x88，四周留 8% 边（线条图标）
#   icon-<id>@132.png   → icon-<id>.bin 132x132，满幅 cover（源图自带圆角/构图）
$iconDefaultSize = 88
$iconSources = @{}
foreach ($png in (Get-ChildItem -LiteralPath $SourceDir -Filter "icon-*.png" -File -ErrorAction SilentlyContinue)) {
    $base = $png.BaseName
    $id = ""
    $px = $iconDefaultSize
    $fill = $false

    if ($base -match '^icon-(?<id>.+?)@(?<px>\d+)$') {
        $id = $Matches['id']
        $px = [int]$Matches['px']
        $fill = $true
    }
    elseif ($base -match '^icon-(?<id>.+)$') {
        $id = $Matches['id']
    }
    else {
        continue
    }

    if ($px -lt 32 -or $px -gt 200) {
        throw ("图标尺寸不合理（{0}，允许 32~200）: {1}" -f $px, $png.Name)
    }
    $iconSources[$id] = [pscustomobject]@{ Path = $png.FullName; Size = $px; Fill = $fill }
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
    $iconSrc = $null
    if ($iconSources.ContainsKey($id)) {
        $iconSrc = $iconSources[$id]
        $icon = "icon-$id.bin"      # 打包时转成 LVGL 二进制，固件里不用 PNG 解码器
        if ($icon.Length -gt 31) {
            throw ("图标名过长（{0} 字符，上限 31）: {1}，请缩短 id" -f $icon.Length, $icon)
        }
    }

    $entries += [pscustomobject]@{
        id       = $id
        name     = $id
        version  = $version
        bin      = $bin.Name
        size     = $bin.Length
        sha256   = $hash
        icon     = $icon
        iconPng  = $(if ($iconSrc -ne $null) { $iconSrc.Path } else { "" })
        iconPx   = $(if ($iconSrc -ne $null) { $iconSrc.Size } else { 0 })
        iconFill = $(if ($iconSrc -ne $null) { $iconSrc.Fill } else { $false })
    }
}

if ($entries.Count -eq 0) {
    Write-Warning "目录里没有 app-*.bin，生成的镜像库是空的"
}

# ---- 共享资源（字库 / 音频素材，不属于任何 app） -----------------------------
# font-cjk.ttf 由 components/cjk_font 在运行时从 storage 读取，selector
# 与所有 app 共用一份，因此不能打进各自的镜像里。
# music-*.wav 是 music app 的音频素材：素材属于数据，放镜像库，
# 换歌只重打 storage，不用重编 app（文件名同样受 SPIFFS 31 字符限制）。
$extras = @()
foreach ($pattern in @("font-*.ttf", "music-*.wav")) {
    foreach ($extra in (Get-ChildItem -LiteralPath $SourceDir -Filter $pattern -File -ErrorAction SilentlyContinue)) {
        if ($extra.Name.Length -gt 31) {
            throw ("文件名过长（{0} 字符，上限 31）: {1}" -f $extra.Name.Length, $extra.Name)
        }
        $extras += [pscustomobject]@{
            Name   = $extra.Name
            Path   = $extra.FullName
            Size   = $extra.Length
            Sha256 = (Get-FileHash -LiteralPath $extra.FullName -Algorithm SHA256).Hash.ToLower()
        }
    }
}

$duplicated = $entries | Group-Object id | Where-Object { $_.Count -gt 1 }
if ($duplicated) {
    throw ("存在重复 app id: " + (($duplicated | ForEach-Object { $_.Name }) -join ", "))
}

# ---- 图标转换：PNG → LVGL 二进制 --------------------------------------------
# 固件里不开 PNG 解码器（省 flash，也不用占 LVGL 的小堆），打包时把
# icon-<id>.png 缩到目标尺寸并转成「12 字节 lv_image_header + BGRA 像素」，
# 运行时从 SPIFFS 读进 PSRAM 直接当 lv_image_dsc_t 用。
function ConvertTo-LvglIcon {
    param([string]$Png, [string]$OutBin, [int]$Size, [switch]$Fill)

    Add-Type -AssemblyName System.Drawing

    $src = [System.Drawing.Image]::FromFile($Png)
    try {
        $bmp = New-Object System.Drawing.Bitmap $Size, $Size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        try {
            $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $g.Clear([System.Drawing.Color]::Transparent)

            if ($Fill) {
                # 满幅（icon-<id>@<px>.png）：cover 缩放，源图自带圆角和构图
                $scale = [Math]::Max($Size / $src.Width, $Size / $src.Height)
                $w = [Math]::Max(1, [int][Math]::Ceiling($src.Width * $scale))
                $h = [Math]::Max(1, [int][Math]::Ceiling($src.Height * $scale))
                $g.DrawImage($src, [int](($Size - $w) / 2), [int](($Size - $h) / 2), $w, $h)
            }
            else {
                # 线条图标：等比缩放并居中，四周留 8% 边距，磁贴里看起来不顶边
                $inner = $Size * 0.84
                $scale = [Math]::Min($inner / $src.Width, $inner / $src.Height)
                $w = [Math]::Max(1, [int]($src.Width * $scale))
                $h = [Math]::Max(1, [int]($src.Height * $scale))
                $g.DrawImage($src, [int](($Size - $w) / 2), [int](($Size - $h) / 2), $w, $h)
            }
        }
        finally {
            $g.Dispose()
        }

        # LVGL 的 ARGB8888 在内存里就是 B,G,R,A 顺序，正好等于 .NET 的 32bppArgb 内存布局
        $rect = New-Object System.Drawing.Rectangle 0, 0, $Size, $Size
        $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                              [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try {
            $stride = $data.Stride
            $bytes = New-Object byte[] ($stride * $Size)
            [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
        }
        finally {
            $bmp.UnlockBits($data)
            $bmp.Dispose()
        }

        # lv_image_header_t（12 字节，位域按小端排）：
        #   magic=0x19(A8 前身魔数) cf=0x10(ARGB8888) flags=0
        #   w,h=stride 见下
        $out = New-Object System.Collections.Generic.List[byte]
        $out.Add(0x19)
        $out.Add(0x10)              # LV_COLOR_FORMAT_ARGB8888
        $out.Add(0x00); $out.Add(0x00)
        $out.Add([byte]($Size -band 0xFF)); $out.Add([byte](($Size -shr 8) -band 0xFF))
        $out.Add([byte]($Size -band 0xFF)); $out.Add([byte](($Size -shr 8) -band 0xFF))
        $out.Add([byte]($stride -band 0xFF)); $out.Add([byte](($stride -shr 8) -band 0xFF))
        $out.Add(0x00); $out.Add(0x00)
        $out.AddRange($bytes)

        [System.IO.File]::WriteAllBytes($OutBin, $out.ToArray())
        return $out.Count
    }
    finally {
        $src.Dispose()
    }
}

# ---- 表情包（公共资源）：faces/face-<name>.png → face-<name>.bin ------------
# 设备统一的拟人形象，selector 与所有 app 共用，素材不编进任何镜像。
# 与图标不同的是：素材是用户手工画的，构图/尺寸各不相同，所以打包时要
#   1) 按不透明像素的包围盒裁掉四周空白
#   2) 等比缩放到同一画布（94%）并居中
# 否则播报换帧时脸会左右跳、大小闪。
$faceSize = 192

function ConvertTo-LvglFace {
    param([string]$Png, [string]$OutBin, [int]$Size)

    Add-Type -AssemblyName System.Drawing

    $src = [System.Drawing.Image]::FromFile($Png)
    try {
        # ---- 包围盒：缩到 128 扫 alpha，误差 3~4 px，画布里还留了 6% 边距 ----
        $probe = 128
        $thumb = New-Object System.Drawing.Bitmap $probe, $probe, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $gt = [System.Drawing.Graphics]::FromImage($thumb)
        try {
            $gt.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $gt.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $gt.Clear([System.Drawing.Color]::Transparent)
            $gt.DrawImage($src, 0, 0, $probe, $probe)
        }
        finally { $gt.Dispose() }

        $thr = New-Object System.Drawing.Rectangle 0, 0, $probe, $probe
        $td = $thumb.LockBits($thr, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                              [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try {
            $tstride = $td.Stride
            $tb = New-Object byte[] ($tstride * $probe)
            [System.Runtime.InteropServices.Marshal]::Copy($td.Scan0, $tb, 0, $tb.Length)
        }
        finally {
            $thumb.UnlockBits($td)
            $thumb.Dispose()
        }

        $minX = $probe; $minY = $probe; $maxX = -1; $maxY = -1
        for ($y = 0; $y -lt $probe; $y++) {
            $row = $y * $tstride
            for ($x = 0; $x -lt $probe; $x++) {
                if ($tb[$row + $x * 4 + 3] -gt 16) {
                    if ($x -lt $minX) { $minX = $x }
                    if ($x -gt $maxX) { $maxX = $x }
                    if ($y -lt $minY) { $minY = $y }
                    if ($y -gt $maxY) { $maxY = $y }
                }
            }
        }

        if ($maxX -lt 0) {
            # 整张全透明（素材有问题）：按原图整幅处理，至少不报错
            $srcRect = [System.Drawing.Rectangle]::new(0, 0, $src.Width, $src.Height)
        }
        else {
            $sx = $src.Width / $probe
            $sy = $src.Height / $probe
            $rx = [int][Math]::Max(0, [Math]::Floor($minX * $sx))
            $ry = [int][Math]::Max(0, [Math]::Floor($minY * $sy))
            $rw = [int][Math]::Min($src.Width - $rx, [Math]::Ceiling(($maxX - $minX + 1) * $sx))
            $rh = [int][Math]::Min($src.Height - $ry, [Math]::Ceiling(($maxY - $minY + 1) * $sy))
            $srcRect = [System.Drawing.Rectangle]::new($rx, $ry, $rw, $rh)
        }

        # ---- 等比缩放到画布 94%，居中，四周留边（圆屏 + 呼吸缩放都不会顶边） ----
        $bmp = New-Object System.Drawing.Bitmap $Size, $Size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        try {
            $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $g.Clear([System.Drawing.Color]::Transparent)

            $inner = $Size * 0.94
            $scale = [Math]::Min($inner / $srcRect.Width, $inner / $srcRect.Height)
            $w = [Math]::Max(1, [int]($srcRect.Width * $scale))
            $h = [Math]::Max(1, [int]($srcRect.Height * $scale))
            $dest = [System.Drawing.Rectangle]::new([int](($Size - $w) / 2), [int](($Size - $h) / 2), $w, $h)
            $g.DrawImage($src, $dest, $srcRect, [System.Drawing.GraphicsUnit]::Pixel)
        }
        finally { $g.Dispose() }

        $rect = New-Object System.Drawing.Rectangle 0, 0, $Size, $Size
        $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                              [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try {
            $stride = $data.Stride
            $bytes = New-Object byte[] ($stride * $Size)
            [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)
        }
        finally {
            $bmp.UnlockBits($data)
            $bmp.Dispose()
        }

        # lv_image_header_t（12 字节，小端）：magic / cf=ARGB8888 / flags / w / h / stride
        $out = New-Object System.Collections.Generic.List[byte]
        $out.Add(0x19)
        $out.Add(0x10)
        $out.Add(0x00); $out.Add(0x00)
        $out.Add([byte]($Size -band 0xFF)); $out.Add([byte](($Size -shr 8) -band 0xFF))
        $out.Add([byte]($Size -band 0xFF)); $out.Add([byte](($Size -shr 8) -band 0xFF))
        $out.Add([byte]($stride -band 0xFF)); $out.Add([byte](($stride -shr 8) -band 0xFF))
        $out.Add(0x00); $out.Add(0x00)
        $out.AddRange($bytes)

        [System.IO.File]::WriteAllBytes($OutBin, $out.ToArray())
        return $out.Count
    }
    finally {
        $src.Dispose()
    }
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
        $iconOut = Join-Path $stage $entry.icon
        $iconBytes = ConvertTo-LvglIcon -Png $entry.iconPng -OutBin $iconOut -Size $entry.iconPx -Fill:$entry.iconFill
        Write-Host ("  图标 {0} {1}x{1} {2,8:N0} B（源 {3}）" -f $entry.icon, $entry.iconPx, $iconBytes, (Split-Path $entry.iconPng -Leaf))
    }
}

foreach ($extra in $extras) {
    Copy-Item -LiteralPath $extra.Path -Destination $stage
    Write-Host ("  共享资源 {0} {1,10:N0} B" -f $extra.Name, $extra.Size)
}

# ---- 表情包（公共资源）：faces/face-*.png → face-*.bin ----------------------
# 素材目录不进镜像（SPIFFS 没有子目录），只把转换后的 bin 平铺进去。
$faceDir = Join-Path $SourceDir "faces"
$faceEntries = @()
if (Test-Path -LiteralPath $faceDir) {
    foreach ($png in (Get-ChildItem -LiteralPath $faceDir -Filter "face-*.png" -File | Sort-Object Name)) {
        $binName = [System.IO.Path]::GetFileNameWithoutExtension($png.Name) + ".bin"
        if ($binName.Length -gt 31) {
            throw ("表情文件名过长（{0} 字符，上限 31）: {1}" -f $binName.Length, $binName)
        }

        $outBin = Join-Path $stage $binName
        $faceBytes = ConvertTo-LvglFace -Png $png.FullName -OutBin $outBin -Size $faceSize
        $faceEntries += [pscustomobject]@{
            name   = [System.IO.Path]::GetFileNameWithoutExtension($png.Name)
            bin    = $binName
            size   = $faceBytes
            sha256 = (Get-FileHash -LiteralPath $outBin -Algorithm SHA256).Hash.ToLower()
        }
        Write-Host ("  表情 {0,-16} {1}px {2,8:N0} B（源 {3}）" -f $binName, $faceSize, $faceBytes, $png.Name)
    }
    if ($faceEntries.Count -eq 0) {
        Write-Warning "faces 目录存在但没有 face-*.png，表情包不会进镜像"
    }
}

$manifest = [pscustomobject]@{
    version         = 1
    partition_table = $partTableTag
    generated       = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    # iconPng 只是打包时的中间信息，不写进 manifest
    apps            = @($entries | Select-Object id, name, version, bin, size, sha256, icon)
    # 字库 / 音频素材是共享资源（不属于某个 app）
    resources       = @($extras | Select-Object name, size, sha256)
    # 表情包也是共享资源，单独列出来方便排查
    faces           = @($faceEntries | Select-Object name, bin, size, sha256)
}
$manifestPath = Join-Path $stage "manifest.json"
$json = $manifest | ConvertTo-Json -Depth 6
[System.IO.File]::WriteAllText($manifestPath, $json, (New-Object System.Text.UTF8Encoding($false)))

Write-Host ("manifest.json: {0} 个 app, partition_table={1}" -f $entries.Count, $partTableTag)
foreach ($entry in $entries) {
    Write-Host ("  {0,-16} {1,-10} {2,10:N0} B  {3}" -f $entry.id, $entry.version, $entry.size, $entry.sha256.Substring(0, 12))
}
if ($faceEntries.Count -gt 0) {
    $faceTotal = ($faceEntries | Measure-Object -Property size -Sum).Sum
    Write-Host ("faces: {0} 张, 合计 {1:N0} B（PSRAM 常驻）" -f $faceEntries.Count, $faceTotal)
}

# ---- 打包 -------------------------------------------------------------------
$storageBin = Join-Path $OutDir "storage.bin"
$spiffsArgs = @(
    $spiffsgen,
    "--page-size", "512",
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
