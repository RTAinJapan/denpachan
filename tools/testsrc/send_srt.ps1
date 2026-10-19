<#
.SYNOPSIS
  FFmpeg のテストパターンを SRT (caller) で送る。denpachan-worker の listener 動作確認用。

.EXAMPLE
  ./send_srt.ps1                          # 1920x1080 60fps を 127.0.0.1:9001 へ
  ./send_srt.ps1 -Port 9002 -Size 3840x2160 -Bitrate 30M
  ./send_srt.ps1 -Codec hevc_nvenc
#>
param(
    [string]$HostName = '127.0.0.1',
    [int]$Port = 9001,
    [string]$Size = '1920x1080',
    [int]$Fps = 60,
    [string]$Bitrate = '8M',
    [string]$Codec = 'libx264',
    [int]$LatencyMs = 200,
    [string]$Passphrase = ''
)
$ErrorActionPreference = 'Stop'

$codecArgs = switch -Wildcard ($Codec) {
    'libx264' { @('-preset', 'veryfast', '-tune', 'zerolatency') }
    '*nvenc' { @('-preset', 'p4', '-tune', 'll') }
    default { @() }
}

$url = "srt://${HostName}:${Port}?mode=caller&latency=$($LatencyMs * 1000)&pkt_size=1316"
if ($Passphrase) { $url += "&passphrase=$Passphrase" }

# 左上に時刻を焼き込み、遅延の目視確認に使う
$video = "testsrc2=size=${Size}:rate=${Fps},drawtext=text='%{localtime\:%H\\\:%M\\\:%S} %{n}':x=40:y=40:fontsize=64:fontcolor=white:box=1:boxcolor=black@0.6"

ffmpeg -hide_banner -re `
    -f lavfi -i $video `
    -f lavfi -i "sine=frequency=1000:sample_rate=48000" `
    -c:v $Codec @codecArgs -b:v $Bitrate -g ($Fps * 2) -bf 0 -pix_fmt yuv420p `
    -c:a aac -b:a 192k `
    -f mpegts $url
