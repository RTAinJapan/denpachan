<#
.SYNOPSIS
  ファイルの形式を確かめる (-Fix で整える)。CI でも実行する。

.DESCRIPTION
  決まり (.editorconfig、.clang-format、.gitattributes) と同じことを、git で管理しているファイルについて確かめる。
  VS Code で保存すれば同じ形になる (.vscode/settings.json)。

  1. すべてのテキストファイル: UTF-8 (BOM は .ps1 だけに付ける)、改行は LF、末尾に改行が 1 つだけ、
     行末に空白が無い、字下げにタブを使わない
  2. C++ (*.cpp, *.h): clang-format (.clang-format) で整えた形と同じ
  3. C# (画面): dotnet format whitespace (.editorconfig) で整えた形と同じ

  clang-format は次の順に探す: 引数 -ClangFormat、環境変数 CLANG_FORMAT、VS Code の C++ 拡張機能に付属のもの、
  Visual Studio に付属のもの、PATH。版 22 と 23 で結果が同じことを確かめてある (CI は pip の 23.1.0)。

.EXAMPLE
  ./tools/format/check.ps1          # 確かめるだけ (合わなければ終了コード 1)
  ./tools/format/check.ps1 -Fix     # 整える
#>
param(
    [switch]$Fix,
    [string]$ClangFormat = ''
)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$problems = [System.Collections.Generic.List[string]]::new()
$fixed = 0

# ---- 1. すべてのテキストファイル ----

$binary = '\.(ico|png)$'
$utf8 = [System.Text.UTF8Encoding]::new($false, $true)  # BOM なし、不正なバイト列は例外
# 管理しているファイルと、まだ add していない新しいファイル (.gitignore で除くものは見ない)
$files = git -C $root ls-files --cached --others --exclude-standard | Sort-Object -Unique
foreach ($rel in $files) {
    if ($rel -match $binary) { continue }
    $path = Join-Path $root $rel
    if (-not (Test-Path $path)) { continue }
    $bytes = [System.IO.File]::ReadAllBytes($path)
    $hasBom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
    $wantBom = $rel -like '*.ps1'
    $offset = if ($hasBom) { 3 } else { 0 }
    try {
        $text = $utf8.GetString($bytes, $offset, $bytes.Length - $offset)
    } catch {
        $problems.Add("${rel}: UTF-8 ではない")
        continue
    }
    $issues = @()
    if ($hasBom -ne $wantBom) { $issues += $(if ($wantBom) { 'BOM が無い' } else { 'BOM がある' }) }
    if ($text.Contains("`r")) { $issues += '改行が LF でない (CR がある)' }
    $lines = ($text -replace "`r", '') -split "`n"
    if ($text.Length -gt 0 -and -not $text.EndsWith("`n")) { $issues += '末尾に改行が無い' }
    if ($text -match "`n`n+\z" -or $text -match "\A`n+\z") { $issues += '末尾に空行がある' }
    $trailing = @($lines | Where-Object { $_ -match '[ \t]+$' }).Count
    if ($trailing -gt 0) { $issues += "行末の空白 ($trailing 行)" }
    $tabs = @($lines | Where-Object { $_ -match '^ *\t' }).Count
    if ($tabs -gt 0) { $issues += "タブの字下げ ($tabs 行)" }
    if ($issues.Count -eq 0) { continue }

    if ($Fix) {
        $body = ($lines | ForEach-Object { ($_ -replace '[ \t]+$', '') -replace '^( *)\t', '$1    ' }) -join "`n"
        $body = $body.TrimEnd("`n") + "`n"
        [System.IO.File]::WriteAllText($path, $body, [System.Text.UTF8Encoding]::new($wantBom))
        $fixed++
        Write-Host "fixed: $rel ($($issues -join ', '))"
    } else {
        $problems.Add("${rel}: $($issues -join ', ')")
    }
}

# ---- 2. C++ (clang-format) ----

function Find-ClangFormat {
    if ($ClangFormat) { return $ClangFormat }
    if ($env:CLANG_FORMAT) { return $env:CLANG_FORMAT }
    $ext = Get-ChildItem (Join-Path $env:USERPROFILE '.vscode\extensions') -Directory -Filter 'ms-vscode.cpptools-*' -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName 'LLVM\bin\clang-format.exe' } | Where-Object { Test-Path $_ } |
        Select-Object -First 1
    if ($ext) { return $ext }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -property installationPath
        if ($vs) {
            $p = Join-Path $vs 'VC\Tools\Llvm\x64\bin\clang-format.exe'
            if (Test-Path $p) { return $p }
        }
    }
    $cmd = Get-Command clang-format -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

$cf = Find-ClangFormat
if (-not $cf) {
    $problems.Add('clang-format が見つからない (VS Code の C++ 拡張機能か Visual Studio の C++ を入れる。または -ClangFormat で指定)')
} else {
    Write-Host "clang-format: $cf ($((& $cf --version) -replace '\s*\(.*$', ''))"
    $style = "--style=file:$(Join-Path $root '.clang-format')"
    foreach ($rel in ($files | Where-Object { $_ -match '\.(cpp|h)$' })) {
        $path = Join-Path $root $rel
        $formatted = (& $cf $style $path) -join "`n"
        $current = ([System.IO.File]::ReadAllText($path) -replace "`r", '').TrimEnd("`n")
        if ($formatted.TrimEnd("`n") -eq $current) { continue }
        if ($Fix) {
            & $cf $style -i $path
            $fixed++
            Write-Host "fixed: $rel (clang-format)"
        } else {
            $problems.Add("${rel}: clang-format と違う")
        }
    }
}

# ---- 3. C# (dotnet format) ----

$csproj = Join-Path $root 'src/ui/Denpachan.Ui/Denpachan.Ui.csproj'
if ($Fix) {
    dotnet format whitespace $csproj
    if ($LASTEXITCODE) { $problems.Add('dotnet format whitespace が失敗した') }
} else {
    $out = dotnet format whitespace $csproj --verify-no-changes 2>&1
    if ($LASTEXITCODE) {
        $problems.Add("C#: dotnet format whitespace と違う`n$(($out | Select-Object -First 20) -join "`n")")
    }
}

if ($Fix) { Write-Host "整えたファイル: $fixed" }
if ($problems.Count -gt 0) {
    Write-Host ''
    Write-Host "形式が合わない: $($problems.Count) 件 (./tools/format/check.ps1 -Fix で整える)"
    $problems | ForEach-Object { Write-Host "- $_" }
    exit 1
}
Write-Host 'OK: ファイルの形式はそろっている'
