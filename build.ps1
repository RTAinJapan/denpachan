<#
.SYNOPSIS
  denpachan の C++ 部分をビルドする。

.DESCRIPTION
  Visual Studio の開発者シェルに入り、CMake プリセットで configure / build する。
  VCPKG_ROOT が未設定なら D:\dev\vcpkg を使う。vcpkg のダウンロード / バイナリキャッシュは、
  未設定なら VCPKG_ROOT の隣の vcpkg-cache 配下に置く (C ドライブを圧迫しないため)。

.EXAMPLE
  ./build.ps1                 # Release
  ./build.ps1 -Config debug
#>
param(
    [ValidateSet('release', 'debug')]
    [string]$Config = 'release',
    [switch]$Clean
)
$ErrorActionPreference = 'Stop'

# 呼び出し元が指定した vcpkg (開発者シェルに入る前の値)
$vcpkgRoot = $env:VCPKG_ROOT

# VS 開発者シェル (cl.exe / cmake / ninja を PATH に入れる)
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'C++ ツールセット入りの Visual Studio が見つからない' }
    Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}

# 開発者シェルは VCPKG_ROOT を VS 同梱の vcpkg に書き換えるので、シェルに入った後で設定し直す
$env:VCPKG_ROOT = if ($vcpkgRoot) { $vcpkgRoot } else { 'D:\dev\vcpkg' }
if (-not (Test-Path "$env:VCPKG_ROOT\vcpkg.exe")) { throw "vcpkg が見つからない: $env:VCPKG_ROOT" }
$cacheRoot = Join-Path (Split-Path $env:VCPKG_ROOT -Parent) 'vcpkg-cache'
if (-not $env:VCPKG_DEFAULT_BINARY_CACHE) {
    $env:VCPKG_DEFAULT_BINARY_CACHE = Join-Path $cacheRoot 'archives'
    New-Item -ItemType Directory -Force $env:VCPKG_DEFAULT_BINARY_CACHE | Out-Null
}
if (-not $env:VCPKG_DOWNLOADS) {
    $env:VCPKG_DOWNLOADS = Join-Path $cacheRoot 'downloads'
    New-Item -ItemType Directory -Force $env:VCPKG_DOWNLOADS | Out-Null
}

Push-Location $PSScriptRoot
try {
    if ($Clean -and (Test-Path out/build)) { Remove-Item -Recurse -Force out/build }
    cmake --preset x64
    if ($LASTEXITCODE) { throw 'configure failed' }
    cmake --build --preset $Config
    if ($LASTEXITCODE) { throw 'build failed' }
} finally {
    Pop-Location
}
