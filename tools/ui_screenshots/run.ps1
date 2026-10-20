<#
.SYNOPSIS
  画面の試験: 試験用の設定・状態 (cases/*.json) を画面に流し込んでスクリーンショットを撮り、
  画面の状態が期待どおりかを確かめる。engine や FFmpeg は要らない。

.DESCRIPTION
  build.ps1 のあとに実行する。denpachan.exe --fixture CASE --screenshot PNG で 1 枚ずつ撮る。
  denpachan.exe は PNG の隣に画面の状態 (PNG.txt) を書き、case の expect と合わなければ終了コード 1 で終わる。
  ここでは終了コードと、PNG がまともな大きさで書けたことを確かめる。
  出力は out/ui-screenshots/ (CI では artifact "ui-screenshots" になる)。

  -Language を付けると、その言語 (Lang/*.json の言語コード) で撮る。期待値は日本語の文言で書いてあるので、
  このときは期待値と比べず (合わないのは失敗にしない)、撮るだけにする。見切れなどは PNG を見て確かめる。

.EXAMPLE
  ./tools/ui_screenshots/run.ps1
  ./tools/ui_screenshots/run.ps1 -Only degraded     # 名前に degraded を含む撮影だけ
  ./tools/ui_screenshots/run.ps1 -Language en       # 英語で撮る (out/ui-screenshots-en/)
#>
param(
    [string]$Bin,
    [string]$Out,
    [string]$Only = '',
    [string]$Language = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $Bin) { $Bin = Join-Path $root 'out/build/bin/Release' }
if (-not $Out) { $Out = Join-Path $root ($(if ($Language) { "out/ui-screenshots-$Language" } else { 'out/ui-screenshots' })) }
$exe = Join-Path $Bin 'denpachan.exe'
if (-not (Test-Path $exe)) { throw "先に build.ps1 でビルドする ($exe)" }
$cases = Join-Path $PSScriptRoot 'cases'
New-Item -ItemType Directory -Force $Out | Out-Null
Get-ChildItem $Out -Filter '*.png' | Remove-Item -Force
Get-ChildItem $Out -Filter '*.png.txt' | Remove-Item -Force

# 撮るもの: 名前、流し込む case、denpachan.exe への追加の引数。
# --size は中身を描く大きさ (CI の画面はメイン画面の既定 1600x1000 より小さいので、ウィンドウの大きさに頼らない)
$main = @('--size', '1600x1000')
$dialog = @('--size', '600x0')  # 設定画面: 幅 600、高さは中身なり
$shots = @(
    @{ Name = 'main-normal';          Case = 'normal';          Args = $main },
    @{ Name = 'main-degraded';        Case = 'degraded';        Args = $main },
    @{ Name = 'main-ffmpeg-missing';  Case = 'ffmpeg-missing';  Args = $main },
    @{ Name = 'main-disconnected';    Case = 'disconnected';    Args = $main },
    @{ Name = 'main-unreachable';     Case = 'unreachable';     Args = $main },
    @{ Name = 'main-eight';           Case = 'eight';           Args = $main },
    @{ Name = 'settings-general-eight'; Case = 'eight';         Args = $dialog + @('--general') },
    @{ Name = 'settings-input';       Case = 'normal';          Args = $dialog + @('--settings', '1') },
    @{ Name = 'settings-record';      Case = 'normal';          Args = $dialog + @('--settings', '1', '--settings-tab', '1') },
    @{ Name = 'settings-general';     Case = 'normal';          Args = $dialog + @('--general') }
)

function Get-PngSize([string]$path) {
    # IHDR: 16 バイト目から幅と高さ (ビッグエンディアン 4 バイトずつ)
    $b = [System.IO.File]::ReadAllBytes($path)
    if ($b.Length -lt 24 -or $b[1] -ne 0x50 -or $b[2] -ne 0x4E -or $b[3] -ne 0x47) { return $null }
    # [byte] のままシフトすると byte に切り詰められる (1600 が 64 になる) ので int にしてから
    $w = ([int]$b[16] -shl 24) -bor ([int]$b[17] -shl 16) -bor ([int]$b[18] -shl 8) -bor [int]$b[19]
    $h = ([int]$b[20] -shl 24) -bor ([int]$b[21] -shl 16) -bor ([int]$b[22] -shl 8) -bor [int]$b[23]
    return @($w, $h)
}

$failed = @()
foreach ($shot in $shots) {
    if ($Only -and $shot.Name -notlike "*$Only*") { continue }
    $png = Join-Path $Out "$($shot.Name).png"
    $case = Join-Path $cases "$($shot.Case).json"
    # 5 秒待つ: 状態バーの CPU 使用率は 2 回目の採取 (2 秒ごと) から出る
    $args = @('--fixture', $case, '--screenshot', $png, '--delay', '5') + @($shot.Args)
    if ($Language) { $args += @('--language', $Language) }
    # CI の画面ではまれに固まって終わらないことがある (手元では再現しない)。時間切れは 1 回だけやり直し、
    # そのときのウィンドウのタイトル (ダイアログで止まっていればその名前) を残す
    $p = $null
    $timeouts = @()
    foreach ($attempt in 1..2) {
        $p = Start-Process -FilePath $exe -ArgumentList $args -PassThru -NoNewWindow
        if ($p.WaitForExit(60000)) { break }
        $p.Refresh()
        $timeouts += "試行 $attempt で 60 秒で終わらなかった (ウィンドウ: '$($p.MainWindowTitle)')"
        $p.Kill()
        $p.WaitForExit(5000) | Out-Null
        $p = $null
    }
    if ($timeouts.Count -gt 0) { $timeouts | ForEach-Object { Write-Host "warn: $($shot.Name): $_" } }
    if (-not $p) {
        $failed += "$($shot.Name): $($timeouts -join ' / ')"
        continue
    }
    $report = "$png.txt"
    # 終了コード 1 = 期待値と合わない。-Language のときは期待値 (日本語) と比べないので失敗にしない
    if ($p.ExitCode -ne 0 -and -not ($Language -and $p.ExitCode -eq 1)) {
        $detail = if (Test-Path $report) { (Get-Content $report -Raw) } else { '(状態の書き出しなし)' }
        $failed += "$($shot.Name): 終了コード $($p.ExitCode)`n$detail"
        continue
    }
    if (-not (Test-Path $png)) { $failed += "$($shot.Name): PNG がない"; continue }
    $size = Get-PngSize $png
    if (-not $size -or $size[0] -lt 400 -or $size[1] -lt 300) {
        $failed += "$($shot.Name): PNG が小さすぎる ($($size -join 'x'))"
        continue
    }
    Write-Host ("OK: {0} ({1}x{2}, {3:N0} bytes)" -f $shot.Name, $size[0], $size[1], (Get-Item $png).Length)
}

if ($failed.Count -gt 0) {
    Write-Host ''
    Write-Host "失敗: $($failed.Count) 件"
    $failed | ForEach-Object { Write-Host "- $_" }
    exit 1
}
Write-Host "すべて OK ($Out)"
