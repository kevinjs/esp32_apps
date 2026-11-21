<#
.SYNOPSIS
    把一张透明底的卡通形象做成 selector 磁贴图标：圆角矩形卡片 + 人物头像。

.DESCRIPTION
    selector 的磁贴是 132x132 圆角方块，图标按 88x88 居中显示
    （pack_storage.ps1 把 icon-<id>.png 缩成 88x88 的 icon-<id>.bin）。
    这里生成 256x256 的源图，缩小后仍有足够采样：

      1. 画一个圆角矩形卡片（渐变底色 + 亮色描边）
      2. 从源图裁出上部（头部），等比缩放、水平居中、底部对齐放进卡片

    源图的透明包围盒由脚本自动探测，所以素材四周留白多少都不影响。

.PARAMETER Source
    透明底 PNG，通常是 storage_src/faces/face-idle.png 这类表情素材

.PARAMETER Out
    输出 PNG，放在 storage_src/icon-<id>.png

.EXAMPLE
    .\make_app_icon.ps1 -Source ..\storage_src\faces\face-idle.png `
                        -Out ..\storage_src\icon-xiaozhi.png
#>
param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$Size = 256,
    [double]$RadiusRatio = 0.22,        # 圆角半径 / 边长（88 px 显示时约 19 px）
    [string]$FillTop = "#2A2150",
    [string]$FillBottom = "#150F26",
    [string]$Stroke = "#8D7BFF",
    [int]$StrokeWidth = 4,
    [double]$SubjectCrop = 0.62,        # 只取源图上部多少比例（头部），1 = 整图
    [double]$SubjectWidthRatio = 0.86,  # 头像占卡片宽度的比例（非 cover 模式）
    [switch]$SubjectCover,              # 头像铺满整张卡片（cover + 圆角裁切）
    [switch]$NoCard                     # 不要卡片底/描边，只留圆角裁切的头像
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

function ConvertTo-Color([string]$hex) {
    $h = $hex.TrimStart('#')
    return [System.Drawing.Color]::FromArgb(
        255,
        [Convert]::ToInt32($h.Substring(0, 2), 16),
        [Convert]::ToInt32($h.Substring(2, 2), 16),
        [Convert]::ToInt32($h.Substring(4, 2), 16))
}

function New-RoundRectPath([single]$x, [single]$y, [single]$w, [single]$h, [single]$r) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $path.AddArc($x, $y, $d, $d, 180, 90)
    $path.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $path.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $path.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

$src = [System.Drawing.Image]::FromFile((Resolve-Path -LiteralPath $Source).Path)
try {
    # ---- 透明包围盒（缩到 128 扫 alpha，误差 3~4 px 足够） ----
    $probe = 128
    $thumb = New-Object System.Drawing.Bitmap $probe, $probe, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $gt = [System.Drawing.Graphics]::FromImage($thumb)
    try {
        $gt.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $gt.Clear([System.Drawing.Color]::Transparent)
        $gt.DrawImage($src, 0, 0, $probe, $probe)
    }
    finally { $gt.Dispose() }

    $tr = New-Object System.Drawing.Rectangle 0, 0, $probe, $probe
    $td = $thumb.LockBits($tr, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
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
        throw "源图看起来是全透明的: $Source"
    }

    $sx = $src.Width / $probe
    $sy = $src.Height / $probe
    $bx = [int][Math]::Max(0, [Math]::Floor($minX * $sx))
    $by = [int][Math]::Max(0, [Math]::Floor($minY * $sy))
    $bw = [int][Math]::Min($src.Width - $bx, [Math]::Ceiling(($maxX - $minX + 1) * $sx))
    $bh = [int][Math]::Min($src.Height - $by, [Math]::Ceiling(($maxY - $minY + 1) * $sy))

    # 只取上部（头部），避免肩膀/衣服把卡片塞满
    if ($SubjectCrop -gt 0 -and $SubjectCrop -lt 1) {
        $bh = [int][Math]::Max(16, $bh * $SubjectCrop)
    }
    $srcRect = New-Object System.Drawing.Rectangle $bx, $by, $bw, $bh

    # ---- 卡片 ----
    $bmp = New-Object System.Drawing.Bitmap $Size, $Size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    try {
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $g.Clear([System.Drawing.Color]::Transparent)

        # 不要卡片时头像直接顶到画布边缘（88 px 显示时最大化）
        $pad = if ($NoCard) { 0 } else { [int]($Size * 0.03) }
        $rect = New-Object System.Drawing.Rectangle $pad, $pad, ($Size - 2 * $pad), ($Size - 2 * $pad)
        $radius = [single]($Size * $RadiusRatio)
        $path = New-RoundRectPath $rect.X $rect.Y $rect.Width $rect.Height $radius

        if (-not $NoCard) {
            $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
                $rect, (ConvertTo-Color $FillTop), (ConvertTo-Color $FillBottom), 90.0)
            try { $g.FillPath($brush, $path) } finally { $brush.Dispose() }
        }

        if ($NoCard -or $SubjectCover) {
            # 铺满模式：cover 缩放 + 圆角裁切，脸占满整张卡片，边缘被圆角吃掉
            $g.SetClip($path)
            $scale = [Math]::Max($rect.Width / $srcRect.Width, $rect.Height / $srcRect.Height)
            $dw = [int][Math]::Ceiling($srcRect.Width * $scale)
            $dh = [int][Math]::Ceiling($srcRect.Height * $scale)
            $dx = $rect.X + [int](($rect.Width - $dw) / 2)
            $dy = $rect.Y + [int](($rect.Height - $dh) / 2)
            $dest = New-Object System.Drawing.Rectangle $dx, $dy, $dw, $dh
            $g.DrawImage($src, $dest, $srcRect, [System.Drawing.GraphicsUnit]::Pixel)
            $g.ResetClip()

            # 描边画在头像之上，卡片轮廓才清楚（NoCard 时不要轮廓）
            if (-not $NoCard) {
                $pen = New-Object System.Drawing.Pen (ConvertTo-Color $Stroke), $StrokeWidth
                try { $g.DrawPath($pen, $path) } finally { $pen.Dispose() }
            }
        }
        else {
            # 卡片内留边：先按宽度算，超高再按高度收，底部对齐（像证件照）
            $innerW = $rect.Width * $SubjectWidthRatio
            $innerH = $rect.Height * 0.88
            $scale = [Math]::Min($innerW / $srcRect.Width, $innerH / $srcRect.Height)
            $dw = [int]($srcRect.Width * $scale)
            $dh = [int]($srcRect.Height * $scale)
            $dx = $rect.X + [int](($rect.Width - $dw) / 2)
            $dy = $rect.Bottom - [int]($rect.Height * 0.06) - $dh
            $dest = New-Object System.Drawing.Rectangle $dx, $dy, $dw, $dh
            $g.DrawImage($src, $dest, $srcRect, [System.Drawing.GraphicsUnit]::Pixel)

            $pen = New-Object System.Drawing.Pen (ConvertTo-Color $Stroke), $StrokeWidth
            try { $g.DrawPath($pen, $path) } finally { $pen.Dispose() }
        }
    }
    finally { $g.Dispose() }

    $dir = Split-Path -Parent $Out
    if ($dir -and -not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    $bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()

    Write-Host ("图标已生成: {0} ({1}x{1}, 源 {2} 裁切 {3}x{4})" -f $Out, $Size, $Source, $srcRect.Width, $srcRect.Height)
}
finally {
    $src.Dispose()
}
