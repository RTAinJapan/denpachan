<#
.SYNOPSIS
  配布用の zip を作る (展開すればすぐ使える形)。

.DESCRIPTION
  build.ps1 のあとに実行する。out/build/bin/Release から配布物を集めて、
  out/dist/denpachan-<版>-win64.zip (中に denpachan-<版>/ フォルダ) と、PDB だけの
  out/dist/denpachan-<版>-win64-symbols.zip を作る。
  FFmpeg は同梱しない (README.md と docs/dependencies.md)。入っていたら止める。

.EXAMPLE
  ./tools/package.ps1                      # 版は CMakeLists.txt から、後ろに git の短いハッシュを付ける
  ./tools/package.ps1 -Suffix ''           # ハッシュを付けない
#>
param(
    [string]$Suffix
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$bin = Join-Path $root 'out/build/bin/Release'
if (-not (Test-Path (Join-Path $bin 'denpachan.exe'))) { throw "先に build.ps1 でビルドする ($bin)" }

$version = (Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern 'project\(denpachan .*VERSION ([0-9.]+)').Matches[0].Groups[1].Value
# [string] の引数は省略しても $null にならないので、指定されたかどうかで見る
if (-not $PSBoundParameters.ContainsKey('Suffix')) { $Suffix = (git -C $root rev-parse --short HEAD).Trim() }
$name = if ($Suffix) { "denpachan-$version-$Suffix" } else { "denpachan-$version" }

$dist = Join-Path $root 'out/dist'
$stage = Join-Path $dist $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage, (Join-Path $stage 'docs') | Out-Null

# 実行ファイル・DLL・ライセンス (PDB やリンク用の中間ファイル、開発用のツールは入れない)
$exclude = '*.pdb', '*.ilk', '*.exp', '*.lib', 'audio_probe.exe'
Get-ChildItem $bin | Where-Object { $n = $_.Name; -not ($exclude | Where-Object { $n -like $_ }) } |
    Copy-Item -Destination $stage -Recurse
Copy-Item (Join-Path $root 'README.md') $stage
foreach ($doc in 'design.md', 'dependencies.md', 'third_party_notices.md') {
    Copy-Item (Join-Path $root "docs/$doc") (Join-Path $stage 'docs')
}

# FFmpeg を同梱していないこと
$ffmpeg = Get-ChildItem $stage -Recurse -File | Where-Object { $_.Name -match '^(avcodec|avformat|avutil|avfilter|avdevice|swresample|swscale|postproc)-\d+\.dll$|^ffmpeg\.exe$' }
if ($ffmpeg) { throw "FFmpeg must not be packaged: $($ffmpeg.Name -join ', ')" }

# Windows PowerShell 5.1 の Compress-Archive や ZipFile.CreateFromDirectory はパスの区切りに \ を使う
# (展開するツールによってはフォルダにならない)。エントリ名を / 区切りにして自分で書く
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
function New-Zip([string]$zipPath, [System.IO.FileInfo[]]$files, [string]$baseDir, [string]$prefix) {
    $stream = [IO.File]::Open($zipPath, [IO.FileMode]::Create)
    $archive = New-Object IO.Compression.ZipArchive($stream, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($f in $files) {
            $rel = $f.FullName.Substring($baseDir.Length).TrimStart('\', '/').Replace('\', '/')
            $entry = if ($prefix) { "$prefix/$rel" } else { $rel }
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, $entry,
                [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally {
        $archive.Dispose()
        $stream.Dispose()
    }
}

$zip = Join-Path $dist "$name-win64.zip"
$symbols = Join-Path $dist "$name-win64-symbols.zip"
Remove-Item $zip, $symbols -ErrorAction SilentlyContinue
New-Zip $zip (Get-ChildItem $stage -Recurse -File) (Resolve-Path $stage).Path $name
New-Zip $symbols (Get-ChildItem $bin -Filter *.pdb -File) (Resolve-Path $bin).Path ''
Remove-Item -Recurse -Force $stage
Write-Host "package: $zip"
Write-Host "symbols: $symbols"
