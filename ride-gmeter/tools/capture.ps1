<#
ride-gmeter serial capture + statistics (ASCII only, PS 5.1 compatible).

Usage (close idf.py monitor first):
    .\capture.ps1 -Port COM3 -Seconds 15

There is a 3 s countdown, then move the board for the capture window:
    1) push forward/backward horizontally  (accel / brake)
    2) tilt left/right                     (cornering)
    3) tap the table lightly               (road vibration)

Output:
    - per-axis min/max/avg and top-5 peaks on screen
    - raw data saved to capture_<timestamp>.csv
#>
param(
    [string]$Port = "COM3",
    [int]$Seconds = 15,
    [int]$Baud = 115200
)

$ErrorActionPreference = "Stop"

Write-Host "capture starts in 3 s, duration $Seconds s ..." -ForegroundColor Cyan
Start-Sleep -Seconds 3
Write-Host "GO - keep moving the board" -ForegroundColor Green

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$sp.ReadTimeout = 500
$sp.Open()

$sb = New-Object System.Text.StringBuilder
$until = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $until) {
    try { $sb.Append($sp.ReadExisting()) | Out-Null } catch { }
    Start-Sleep -Milliseconds 50
}
$sp.Close()

$raw = $sb.ToString()
$lines = $raw -split "`r?`n" | Where-Object { $_ -match '^\d' }

if ($lines.Count -eq 0) {
    Write-Host "no data: check that ride-gmeter is flashed and the port is free" -ForegroundColor Red
    exit 1
}

$csv = Join-Path $PSScriptRoot ("capture_" + (Get-Date).ToString('yyyyMMdd_HHmmss') + ".csv")
"t_ms,ax,ay,az,abs_a,gx,gy,gz" | Set-Content -Path $csv -Encoding ASCII
$lines | Add-Content -Path $csv -Encoding ASCII

$rows = $lines | ForEach-Object {
    $f = $_.Split(',')
    [pscustomobject]@{
        t  = [double]$f[0]
        ax = [double]$f[1]; ay = [double]$f[2]; az = [double]$f[3]
        am = [double]$f[4]
        gx = [double]$f[5]; gy = [double]$f[6]; gz = [double]$f[7]
    }
}

Write-Host ""
Write-Host ("samples: {0}  duration: {1} s" -f $rows.Count, $Seconds) -ForegroundColor Cyan
Write-Host ("{0,-4} {1,9} {2,9} {3,9}" -f "axis", "min", "max", "avg")
foreach ($k in 'ax', 'ay', 'az', 'am', 'gx', 'gy', 'gz') {
    $s = $rows | Measure-Object $k -Average -Minimum -Maximum
    Write-Host ("{0,-4} {1,9:N3} {2,9:N3} {3,9:N3}" -f $k, $s.Minimum, $s.Maximum, $s.Average)
}

$active = ($rows | Where-Object { $_.am -gt 0.5 }).Count
Write-Host ""
Write-Host ("active samples (abs_a > 0.5 m/s2): {0}" -f $active) -ForegroundColor Yellow

Write-Host "top 5 peaks:" -ForegroundColor Yellow
$rows | Sort-Object am -Descending | Select-Object -First 5 |
    ForEach-Object { Write-Host ("  t={0,7} abs_a={1,6:N2}  ax={2,6:N2} ay={3,6:N2} az={4,6:N2}" -f $_.t, $_.am, $_.ax, $_.ay, $_.az) }

Write-Host ""
Write-Host "saved: $csv" -ForegroundColor Cyan
