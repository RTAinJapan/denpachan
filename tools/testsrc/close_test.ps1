<#
.SYNOPSIS
  画面を閉じたときの動きの試験 (閉じると受信も止める。受信中なら確認する)。

.DESCRIPTION
  試験用の設定 (out/check/close-config.json) で engine を先に起動し、画面 (denpachan.exe) を開いて閉じる。
  確認ダイアログは UI オートメーションで読み、ボタンを押す。
    A. 受信中に閉じる → 確認が出る → キャンセル → 画面も engine も動いたまま
    B. 受信中に閉じる → 確認 → OK → 画面・engine・worker が終わる
    C. 受信中のチャンネルがないときに閉じる → 確認なしで画面と engine が終わる
    D. engine が落ちた直後 (繋がっていない間) に閉じる → 画面が起動し直した engine に繋がってから止め、画面も engine も終わる
    E. 画面が engine を起動し直した直後 (まだ繋がっていない) に閉じる → 起動し直した engine も止まる
  build.ps1 のあとに実行する。FFmpeg は tools/testsrc/dev_env.sh と同じく out/deps のものを使う。

.EXAMPLE
  ./tools/testsrc/close_test.ps1
#>
$ErrorActionPreference = 'Stop'
# 外部コマンド (denpachan-ctl) に JSON を渡すときの " の扱いを、PowerShell 7 でも 5.1 と同じにする
# (下の '{\"name\":...}' は 5.1 の渡し方に合わせて書いてある。7 の既定のままだと \ まで渡って JSON が壊れる)
if ($PSVersionTable.PSVersion.Major -ge 7) { $PSNativeCommandArgumentPassing = 'Legacy' }
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$bin = Join-Path $root 'out/build/bin/Release'
$out = Join-Path $root 'out/check'
$cfg = Join-Path $out 'close-config.json'
$ctl = Join-Path $bin 'denpachan-ctl.exe'
$env:PATH = (Join-Path $root 'out/deps/ffmpeg-9.0.2-full_build-shared/bin') + ';' + $env:PATH
$failed = 0

function Step([string]$text) { Write-Host ''; Write-Host "==== $text" }
function Check([bool]$ok, [string]$text) {
    if ($ok) { Write-Host "OK: $text" } else { Write-Host "FAIL: $text"; $script:failed++ }
}
function Running([string]$name) { @(Get-Process $name -ErrorAction SilentlyContinue).Count }

function Stop-All {
    Get-Process denpachan, denpachan-engine, denpachan-worker -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Seconds 1
}

function Start-Engine {
    Start-Process (Join-Path $bin 'denpachan-engine.exe') -ArgumentList '--config', $cfg, '--log-file', (Join-Path $out 'close-engine.log') -WindowStyle Hidden
    Start-Sleep -Seconds 2
}

function Start-Ui {
    $p = Start-Process (Join-Path $bin 'denpachan.exe') -PassThru
    Start-Sleep -Seconds 5  # engine に繋がって状態が届くまで
    return $p
}

# 確認ダイアログ (タイトル denpachan、ボタンあり) を探す
function Find-Dialog([int]$processId, [int]$timeoutMs = 3000) {
    $deadline = (Get-Date).AddMilliseconds($timeoutMs)
    $cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, $processId)
    while ((Get-Date) -lt $deadline) {
        $windows = [System.Windows.Automation.AutomationElement]::RootElement.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)
        foreach ($w in $windows) {
            # MessageBox は標準のダイアログ (クラス #32770)。メイン画面は拾わない
            if ($w.Current.ControlType -ne [System.Windows.Automation.ControlType]::Window -or $w.Current.ClassName -ne '#32770') { continue }
            $buttons = $w.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                (New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Button)))
            $texts = $w.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                (New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Text)))
            $message = ($texts | ForEach-Object { $_.Current.Name }) -join ' '
            if ($message -match '受信中') { return @{ Window = $w; Buttons = $buttons; Message = $message } }
        }
        Start-Sleep -Milliseconds 200
    }
    return $null
}

function Click([hashtable]$dialog, [string]$pattern) {
    foreach ($b in $dialog.Buttons) {
        if ($b.Current.Name -match $pattern) {
            $b.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
            return
        }
    }
    throw "button '$pattern' not found"
}

Stop-All
Remove-Item $cfg -ErrorAction SilentlyContinue
Start-Engine
# Windows PowerShell 5.1 は外部コマンドに渡すとき " を消すのでエスケープする (空白を含めると引数が分かれる)
& $ctl set 1 '{\"name\":\"CloseTest\"}' | Out-Null
& $ctl start 1

Step 'A. 受信中に閉じる → 確認が出る → キャンセル'
$ui = Start-Ui
$ui.CloseMainWindow() | Out-Null
$dialog = Find-Dialog $ui.Id
Check ($null -ne $dialog) '確認ダイアログが出た'
if ($dialog) {
    Write-Host "  メッセージ: $($dialog.Message)"
    Click $dialog 'キャンセル|Cancel'
    Start-Sleep -Seconds 2
}
Check (-not $ui.HasExited) '画面は開いたまま'
Check ((Running 'denpachan-engine') -eq 1 -and (Running 'denpachan-worker') -eq 1) 'engine と worker は動いたまま'

Step 'B. 受信中に閉じる → OK → 画面・engine・worker が終わる'
$ui.CloseMainWindow() | Out-Null
$dialog = Find-Dialog $ui.Id
Check ($null -ne $dialog) '確認ダイアログが出た'
if ($dialog) { Click $dialog '^OK$' }
Start-Sleep -Seconds 6
Check $ui.HasExited '画面が終わった'
Check ((Running 'denpachan-engine') -eq 0) 'engine が終わった'
Check ((Running 'denpachan-worker') -eq 0) 'worker が終わった'

Step 'C. 受信中のチャンネルがないときに閉じる → 確認なしで終わる'
Stop-All
Start-Engine
& $ctl stop 1
$ui = Start-Ui
$ui.CloseMainWindow() | Out-Null
$dialog = Find-Dialog $ui.Id 2000
Check ($null -eq $dialog) '確認ダイアログは出ない'
Start-Sleep -Seconds 5
Check $ui.HasExited '画面が終わった'
Check ((Running 'denpachan-engine') -eq 0) 'engine が終わった'

Step 'D. engine が落ちた直後 (繋がっていない間) に閉じる → 画面が起動し直した engine に繋がってから止め、画面も engine も終わる'
# 画面は engine が居ないと 1 秒後に自分で起動する。そのとき普段の設定を読まないよう、設定の場所を試験用にする
$env:APPDATA = Join-Path $out 'close-appdata'
New-Item -ItemType Directory -Force $env:APPDATA | Out-Null
Stop-All
Start-Engine
& $ctl stop 1
$ui = Start-Ui
Get-Process denpachan-engine | Stop-Process -Force
Start-Sleep -Milliseconds 300  # 画面が切れたことに気づくまで (起動し直すより前)
$ui.CloseMainWindow() | Out-Null
Start-Sleep -Seconds 10  # 繋がるのを最大 5 秒待ち、engine を止めてから閉じる
Check $ui.HasExited '画面が終わった (閉じられないまま残らない)'
Check ((Running 'denpachan-engine') -eq 0) 'engine が終わった (画面が起動し直した engine も残らない)'

Step 'E. engine が落ち、画面が engine を起動し直した直後 (まだ繋がっていない) に閉じる → 起動し直した engine も止まる'
Stop-All
Start-Engine
& $ctl stop 1
$ui = Start-Ui
Get-Process denpachan-engine | Stop-Process -Force
# 画面は切れてから約 1 秒後に engine を起動し、その約 1 秒後に繋ぐ。その間に閉じる
$deadline = (Get-Date).AddSeconds(5)
while ((Running 'denpachan-engine') -eq 0 -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 50 }
Check ((Running 'denpachan-engine') -eq 1) '画面が engine を起動し直した'
$ui.CloseMainWindow() | Out-Null
Start-Sleep -Seconds 10
Check $ui.HasExited '画面が終わった'
Check ((Running 'denpachan-engine') -eq 0) '起動し直した engine も止まった (画面なしで残らない)'

Stop-All
Write-Host ''
if ($failed -gt 0) { Write-Host "失敗: $failed 件"; exit 1 }
Write-Host 'すべて OK'
