#!/usr/bin/env bash
# UI の表示確認。試験用の設定で engine を起動し、ch1 に SRT、ch2 に RTMP push を送った状態で
# UI を開き、数秒後の画面を PNG に保存する (out/check/ui.png)。
#
#   tools/testsrc/ui_test.sh [delay_sec] [select_ch] [settings_ch]
#     settings_ch を指定すると、そのチャンネルの設定画面を out/check/ui-settings.png (録画タブは
#     ui-settings-record.png) に、全体の設定画面を ui-general.png に撮る
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tools/testsrc/dev_env.sh"
BIN="$ROOT/out/build/bin/Release"
OUT="$ROOT/out/check"
CFG="$OUT/ui-config.json"
CTL="$BIN/denpachan-ctl.exe"
DELAY="${1:-12}"
SELECT="${2:-1}"
mkdir -p "$OUT"
rm -rf "$CFG" "$OUT/ui.png" "$OUT/ui-rec"
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1
taskkill //IM denpachan-rtmpd.exe //F >/dev/null 2>&1

"$BIN/denpachan-engine.exe" --config "$CFG" --log-file "$OUT/ui-engine.log" >/dev/null 2>&1 &
sleep 2
"$CTL" set 1 '{"name":"Game A","audio":{"device":"CABLE Input","startChannel":1}}' >/dev/null
# ch1 は録画する (CPU エンコード)。保存先は out/check/ui-rec
REC_DIR=$(cygpath -w "$OUT/ui-rec" | sed 's/\\/\\\\/g')
"$CTL" set 1 "{\"record\":{\"enabled\":true,\"dir\":\"$REC_DIR\",\"mode\":\"encode\",\"encode\":{\"video\":{\"encoder\":\"cpu\",\"preset\":\"p3\"}}}}" >/dev/null
"$CTL" set 2 '{"name":"Game B","source":{"protocol":"rtmp","mode":"push","app":"live","key":"gameb"}}' >/dev/null
"$CTL" set 3 '{"name":"Game C"}' >/dev/null
"$CTL" start 1
"$CTL" start 2
"$CTL" start 3

ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=1920x1080:rate=60" \
    -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
    -c:v libx264 -preset veryfast -tune zerolatency -b:v 6M -g 120 -bf 0 -pix_fmt yuv420p \
    -c:a aac -b:a 160k -pes_payload_size 0 -muxdelay 0 -muxpreload 0 -t 60 \
    -f mpegts "srt://127.0.0.1:9001?mode=caller&latency=120000&pkt_size=1316" >/dev/null 2>&1 &
SENDER1=$!
ffmpeg -hide_banner -loglevel error -re -f lavfi -i "smptehdbars=size=1280x720:rate=30" \
    -f lavfi -i "sine=frequency=440:sample_rate=48000" \
    -c:v libx264 -preset veryfast -tune zerolatency -b:v 3M -g 60 -bf 0 -pix_fmt yuv420p \
    -c:a aac -b:a 128k -t 60 -f flv "rtmp://127.0.0.1:1935/live/gameb" >/dev/null 2>&1 &
SENDER2=$!
sleep 4

"$BIN/denpachan.exe" --screenshot "$OUT/ui.png" --delay "$DELAY" --select "$SELECT"
if [ -n "${3:-}" ]; then
    "$BIN/denpachan.exe" --screenshot "$OUT/ui-settings.png" --delay 4 --settings "$3"
    "$BIN/denpachan.exe" --screenshot "$OUT/ui-settings-record.png" --delay 4 --settings "$3" --settings-tab 1
    "$BIN/denpachan.exe" --screenshot "$OUT/ui-general.png" --delay 4 --general
fi

# 送出だけを止める (録画の CPU エンコードの ffmpeg は、チャンネルを止めたときに自分で書き終えて終わる)
kill "$SENDER1" "$SENDER2" 2>/dev/null
"$CTL" stop 1 >/dev/null
"$CTL" stop 2 >/dev/null
"$CTL" stop 3 >/dev/null
sleep 3
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
ls -la "$OUT"/ui*.png
echo "recordings (CPU encode, should have a duration):"
find "$OUT/ui-rec" -name "*.mkv" | sort | tail -3 | while read -r f; do
    echo "  ${f#$OUT/ui-rec/} $(ffprobe -v error -show_entries format=duration -of csv=p=0 "$f")"
done
