<#
.SYNOPSIS
  denpachan Cam 1..4 (DirectShow) を登録 / 解除する。

.DESCRIPTION
  既定はマシン全体への登録 (UAC で管理者権限を求める)。
  -User を付けると現在のユーザーだけに登録する (管理者権限不要。ただし管理者権限で動くアプリからは見えない)。

.EXAMPLE
  ./register_vcam.ps1                 # 登録 (マシン全体)
  ./register_vcam.ps1 -Unregister
  ./register_vcam.ps1 -User           # 登録 (ユーザー単位)
#>
param(
    [string]$Dll = (Join-Path $PSScriptRoot '..\out\build\bin\Release\denpachan-vcam.dll'),
    [switch]$User,
    [switch]$Unregister
)
$ErrorActionPreference = 'Stop'
$Dll = (Resolve-Path $Dll).Path

$regArgs = @('/s')
if ($Unregister) { $regArgs += '/u' }
if ($User) { $regArgs += @('/n', '/i:user') }
$regArgs += "`"$Dll`""

if ($User) {
    $p = Start-Process regsvr32.exe -ArgumentList $regArgs -Wait -PassThru
} else {
    $p = Start-Process regsvr32.exe -ArgumentList $regArgs -Verb RunAs -Wait -PassThru
}
if ($p.ExitCode -ne 0) { throw "regsvr32 failed (exit $($p.ExitCode))" }
Write-Host ($(if ($Unregister) { 'unregistered' } else { 'registered' }) + ": $Dll")
