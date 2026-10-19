<#
.SYNOPSIS
  アイコンの元 (assets/denpachan.svg) から assets/denpachan.ico を作る。

.DESCRIPTION
  SVG を Edge (ヘッドレス) で 1024 px に描き、16〜256 px に縮めて 1 つの ico にまとめる (各サイズは PNG で持つ)。
  ico はリポジトリに入れておく (ビルドに Edge を要らないようにする)。SVG を変えたらこれを実行し直して両方をコミットする。
  確認用に各サイズの PNG を out/icon/ に残す。

.EXAMPLE
  ./tools/icon/make_icon.ps1
#>
param(
    [string]$Svg,
    [string]$Ico
)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $Svg) { $Svg = Join-Path $root 'assets/denpachan.svg' }
if (-not $Ico) { $Ico = Join-Path $root 'assets/denpachan.ico' }
$work = Join-Path $root 'out/icon'
New-Item -ItemType Directory -Force $work | Out-Null

$edge = @(
    "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
    "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $edge) { throw 'Microsoft Edge が見つからない' }

# 1024 px で描く (背景は透明)。手元で開いている Edge と混ざらないよう、別のプロファイルで起動する
$big = 1024
$html = Join-Path $work 'render.html'
$svgUri = ([Uri](Resolve-Path $Svg).Path).AbsoluteUri
Set-Content $html -Encoding utf8 -Value "<!doctype html><html><body style='margin:0;background:transparent'><img src='$svgUri' width='$big' height='$big' style='display:block'></body></html>"
$master = Join-Path $work "icon-$big.png"
Remove-Item $master -ErrorAction SilentlyContinue
$profileDir = Join-Path $work 'edge-profile'
$edgeArgs = @('--headless=new', '--disable-gpu', '--hide-scrollbars', '--no-first-run', "--user-data-dir=$profileDir",
              '--default-background-color=00000000', "--window-size=$big,$big", "--screenshot=$master", ([Uri]$html).AbsoluteUri)
Start-Process -FilePath $edge -ArgumentList $edgeArgs -WindowStyle Hidden
# msedge.exe はすぐ終わり、描くのは子プロセスなので、PNG が書き上がるのを待つ
$deadline = (Get-Date).AddSeconds(60)
$lastSize = -1
while ($true) {
    Start-Sleep -Milliseconds 500
    $size = if (Test-Path $master) { (Get-Item $master).Length } else { -1 }
    if ($size -gt 0 -and $size -eq $lastSize) { break }
    $lastSize = $size
    if ((Get-Date) -gt $deadline) { throw "描けなかった ($master)" }
}

# 各サイズに縮める
Add-Type -AssemblyName System.Drawing
$sizes = 256, 128, 64, 48, 32, 24, 16
$source = [System.Drawing.Image]::FromFile($master)
$pngs = @()
try {
    foreach ($size in $sizes) {
        $bmp = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
        $g.Clear([System.Drawing.Color]::Transparent)
        $g.DrawImage($source, 0, 0, $size, $size)
        $g.Dispose()
        $path = Join-Path $work "icon-$size.png"
        $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
        $pngs += , @($size, [System.IO.File]::ReadAllBytes($path))
    }
} finally {
    $source.Dispose()
}

# ico にまとめる: ICONDIR (6 バイト) + ICONDIRENTRY (16 バイト × 枚数) + PNG の中身
$stream = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $stream
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$pngs.Count)
$offset = 6 + 16 * $pngs.Count
foreach ($entry in $pngs) {
    $size, $bytes = $entry
    $dim = if ($size -ge 256) { 0 } else { $size }  # 256 は 0 と書く
    $w.Write([byte]$dim); $w.Write([byte]$dim); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$bytes.Length); $w.Write([uint32]$offset)
    $offset += $bytes.Length
}
foreach ($entry in $pngs) { $w.Write([byte[]]$entry[1]) }
$w.Flush()
[System.IO.File]::WriteAllBytes($Ico, $stream.ToArray())
$w.Dispose()
Write-Host ("書きました: {0} ({1:N0} bytes、{2})" -f $Ico, (Get-Item $Ico).Length, ($sizes -join ' / '))
