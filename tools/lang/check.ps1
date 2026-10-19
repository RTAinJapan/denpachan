<#
.SYNOPSIS
  画面の文言のキーと言語ファイル (src/ui/Denpachan.Ui/Lang/*.json) が食い違っていないかを確かめる。

.DESCRIPTION
  - コードが使うキーが ja.json に無い (画面に [キー] と出てしまう)
  - ja.json にあるのにどこでも使っていないキー (消し忘れ)
  - ほかの言語ファイルに、ja.json に無いキーがある / {name} の引数が ja.json と違う
  を見つけたら終了コード 1。ほかの言語ファイルで訳していないキー (日本語で出る) は数だけ出す。

  コードが使うキーは次から拾う:
  - XAML の {l:Tr キー}
  - C# の文字列リテラルのうち、ja.json のキーと同じ頭 (main. や status. など) で始まるもの
  - C++ の ui_message("ID") と {"id", "ID"} (言語ファイルでは msg.ID)

.EXAMPLE
  ./tools/lang/check.ps1
#>
param([string]$Root)
$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent }
$ui = Join-Path $Root 'src/ui/Denpachan.Ui'
$langDir = Join-Path $ui 'Lang'

function Read-Lang([string]$path) {
    $table = [ordered]@{}
    $json = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    foreach ($p in $json.PSObject.Properties) { $table[$p.Name] = [string]$p.Value }
    return $table
}

function Get-Placeholders([string]$text) {
    return @([regex]::Matches($text, '\{([A-Za-z0-9_]+)\}') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
}

$base = Read-Lang (Join-Path $langDir 'ja.json')
$sections = @($base.Keys | Where-Object { -not $_.StartsWith('_') } | ForEach-Object { $_.Split('.')[0] } | Sort-Object -Unique)
$keyPattern = '^(' + (($sections | ForEach-Object { [regex]::Escape($_) }) -join '|') + ')(\.[A-Za-z0-9]+)+$'

# コードが使うキー (キー → 使っている場所)
$used = @{}
function Add-Used([string]$key, [string]$where) { if (-not $used.ContainsKey($key)) { $used[$key] = $where } }

foreach ($f in Get-ChildItem $ui -Recurse -Filter *.xaml | Where-Object { $_.FullName -notmatch '\\(bin|obj)\\' }) {
    $text = [System.IO.File]::ReadAllText($f.FullName)
    foreach ($m in [regex]::Matches($text, '\{l:Tr\s+([^}\s]+)\s*\}')) { Add-Used $m.Groups[1].Value $f.Name }
}
foreach ($f in Get-ChildItem $ui -Recurse -Filter *.cs | Where-Object { $_.FullName -notmatch '\\(bin|obj)\\' }) {
    $text = [System.IO.File]::ReadAllText($f.FullName)
    foreach ($m in [regex]::Matches($text, '"([A-Za-z0-9_.]+)"')) {
        if ($m.Groups[1].Value -match $keyPattern) { Add-Used $m.Groups[1].Value $f.Name }
    }
}
foreach ($dir in 'common', 'engine', 'worker', 'rtmpd') {
    foreach ($f in Get-ChildItem (Join-Path $Root "src/$dir") -Recurse -Include *.cpp, *.h) {
        $text = [System.IO.File]::ReadAllText($f.FullName)
        foreach ($m in [regex]::Matches($text, 'ui_message\(\s*"([^"]+)"|\{"id",\s*"([^"]+)"\}')) {
            $id = if ($m.Groups[1].Success) { $m.Groups[1].Value } else { $m.Groups[2].Value }
            if ($id -eq 'text') { continue }  # 読めなかったメッセージを文のまま渡す特別な ID (Loc.Message)
            Add-Used "msg.$id" $f.Name
        }
    }
}

$problems = @()
foreach ($key in $used.Keys | Sort-Object) {
    if (-not $base.Contains($key)) { $problems += "ja.json にない: $key ($($used[$key]))" }
}
foreach ($key in $base.Keys) {
    if (-not $key.StartsWith('_') -and -not $used.ContainsKey($key)) { $problems += "ja.json にあるが使っていない: $key" }
}

foreach ($f in Get-ChildItem $langDir -Filter *.json | Where-Object { $_.Name -ne 'ja.json' }) {
    $lang = Read-Lang $f.FullName
    $untranslated = 0
    foreach ($key in $base.Keys) {
        if ($key.StartsWith('_')) { continue }
        if (-not $lang.Contains($key)) { $untranslated++; continue }
        $want = (Get-Placeholders $base[$key]) -join ','
        $have = (Get-Placeholders $lang[$key]) -join ','
        if ($want -ne $have) { $problems += "$($f.Name): $key の引数が ja.json と違う ({$have} / ja.json は {$want})" }
    }
    foreach ($key in $lang.Keys) {
        if (-not $key.StartsWith('_') -and -not $base.Contains($key)) { $problems += "$($f.Name): ja.json にないキー: $key" }
    }
    Write-Host "$($f.Name): 訳していないキー $untranslated 件 (日本語で出る)"
}

if ($problems.Count -gt 0) {
    $problems | ForEach-Object { Write-Host "- $_" }
    Write-Host "失敗: $($problems.Count) 件"
    exit 1
}
Write-Host "OK: キー $($used.Count) 件 (ja.json $($base.Count - 1) 件)"
