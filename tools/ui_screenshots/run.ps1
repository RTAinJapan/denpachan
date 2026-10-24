<#
.SYNOPSIS
  画面の試験: 試験用の設定・状態 (cases/*.json) を画面に流し込んでスクリーンショットを撮り、
  画面の状態が期待どおりかを確かめる。engine や FFmpeg は要らない。

.DESCRIPTION
  build.ps1 のあとに実行する。denpachan.exe --fixture CASE --screenshot PNG で 1 枚ずつ撮る。
  denpachan.exe は PNG の隣に画面の状態 (PNG.txt) を書き、case の expect と合わなければ終了コード 1 で終わる。
  ここでは終了コードと、PNG がまともな大きさで書けたことを確かめる。
  出力は out/ui-screenshots/ (CI では artifact "ui-screenshots" になる)。

  撮るのは -Parallel 枚ずつまとめて (既定 4)。fixture は engine に繋がず、ファイルも PNG とその隣の .txt しか書かないので、
  同時に動かせる。1 枚に 7 秒前後 (起動 + 5 秒待ち) かかるので、順に撮ると枚数分の時間になる。

  -Language を付けると、その言語 (Lang/*.json の言語コード) で撮る。期待値は日本語の文言で書いてあるので、
  このときは期待値と比べず (合わないのは失敗にしない)、撮るだけにする。見切れなどは PNG を見て確かめる。

.EXAMPLE
  ./tools/ui_screenshots/run.ps1
  ./tools/ui_screenshots/run.ps1 -Only degraded     # 名前に degraded を含む撮影だけ
  ./tools/ui_screenshots/run.ps1 -Language en       # 英語で撮る (out/ui-screenshots-en/)
  ./tools/ui_screenshots/run.ps1 -Parallel 1        # 1 枚ずつ撮る (固まったときの切り分け)
#>
param(
    [string]$Bin,
    [string]$Out,
    [string]$Only = '',
    [string]$Language = '',
    [int]$Parallel = 4
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
if ($Parallel -lt 1) { $Parallel = 1 }

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
    @{ Name = 'main-audio';           Case = 'audio';           Args = $main },
    @{ Name = 'settings-general-eight'; Case = 'eight';         Args = $dialog + @('--general') },
    @{ Name = 'settings-input';       Case = 'normal';          Args = $dialog + @('--settings', '1') },
    @{ Name = 'settings-record';      Case = 'normal';          Args = $dialog + @('--settings', '1', '--settings-tab', '1') },
    @{ Name = 'settings-general';     Case = 'normal';          Args = $dialog + @('--general') }
)
$shots = @($shots | Where-Object { -not $Only -or $_.Name -like "*$Only*" })

function Get-PngSize([string]$path) {
    # IHDR: 16 バイト目から幅と高さ (ビッグエンディアン 4 バイトずつ)
    $b = [System.IO.File]::ReadAllBytes($path)
    if ($b.Length -lt 24 -or $b[1] -ne 0x50 -or $b[2] -ne 0x4E -or $b[3] -ne 0x47) { return $null }
    # [byte] のままシフトすると byte に切り詰められる (1600 が 64 になる) ので int にしてから
    $w = ([int]$b[16] -shl 24) -bor ([int]$b[17] -shl 16) -bor ([int]$b[18] -shl 8) -bor [int]$b[19]
    $h = ([int]$b[20] -shl 24) -bor ([int]$b[21] -shl 16) -bor ([int]$b[22] -shl 8) -bor [int]$b[23]
    return @($w, $h)
}

# 1 枚を撮る denpachan.exe を起こす (終わるのは待たない)
function Start-Shot($shot) {
    $png = Join-Path $Out "$($shot.Name).png"
    $case = Join-Path $cases "$($shot.Case).json"
    # 5 秒待つ: 状態バーの CPU 使用率は 2 回目の採取 (2 秒ごと) から出る
    $exeArgs = @('--fixture', $case, '--screenshot', $png, '--delay', '5') + @($shot.Args)
    if ($Language) { $exeArgs += @('--language', $Language) }
    return Start-Process -FilePath $exe -ArgumentList $exeArgs -PassThru -NoNewWindow
}

# 終わらなかった denpachan.exe を止め、そのときのウィンドウのタイトル (ダイアログで止まっていればその名前) を返す
function Stop-Stuck($p) {
    $p.Refresh()
    $title = $p.MainWindowTitle
    $p.Kill()
    $p.WaitForExit(5000) | Out-Null
    return $title
}

$failed = @()
for ($i = 0; $i -lt $shots.Count; $i += $Parallel) {
    # $Parallel 枚をまとめて起こしてから、順に終わるのを待つ
    $batch = @($shots[$i..([Math]::Min($i + $Parallel, $shots.Count) - 1)])
    $running = @($batch | ForEach-Object { @{ Shot = $_; Process = (Start-Shot $_); Timeouts = @() } })
    foreach ($r in $running) {
        $shot = $r.Shot
        # CI の画面ではまれに固まって終わらないことがある (手元では再現しない)。時間切れは 1 回だけ、今度は単独でやり直す
        if (-not $r.Process.WaitForExit(60000)) {
            $r.Timeouts += "試行 1 で 60 秒で終わらなかった (ウィンドウ: '$(Stop-Stuck $r.Process)')"
            $r.Process = Start-Shot $shot
            if (-not $r.Process.WaitForExit(60000)) {
                $r.Timeouts += "試行 2 で 60 秒で終わらなかった (ウィンドウ: '$(Stop-Stuck $r.Process)')"
                $r.Process = $null
            }
        }
        if ($r.Timeouts.Count -gt 0) { $r.Timeouts | ForEach-Object { Write-Host "warn: $($shot.Name): $_" } }
        if (-not $r.Process) {
            $failed += "$($shot.Name): $($r.Timeouts -join ' / ')"
            continue
        }
        $png = Join-Path $Out "$($shot.Name).png"
        $report = "$png.txt"
        # 終了コード 1 = 期待値と合わない。-Language のときは期待値 (日本語) と比べないので失敗にしない
        if ($r.Process.ExitCode -ne 0 -and -not ($Language -and $r.Process.ExitCode -eq 1)) {
            $detail = if (Test-Path $report) { (Get-Content $report -Raw) } else { '(状態の書き出しなし)' }
            $failed += "$($shot.Name): 終了コード $($r.Process.ExitCode)`n$detail"
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
}

if ($failed.Count -gt 0) {
    Write-Host ''
    Write-Host "失敗: $($failed.Count) 件"
    $failed | ForEach-Object { Write-Host "- $_" }
    exit 1
}
Write-Host "すべて OK ($Out)"
