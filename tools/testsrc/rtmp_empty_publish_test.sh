#!/usr/bin/env bash
# 中身を送らずに終わった RTMP の publish のあとの試験 (送出側が配信開始の直後に失敗した、など)。
#
# rtmp_double_publish.py で ch1key に publish だけして切り、そのあと FFmpeg で 6 秒 publish する。
# worker が読み始めに失敗し続けず (rtmpd への再同期の依頼がほぼない)、映像が出ることを確かめる。
# 試験用の設定ファイル (out/check/rtmp3-config.json) を使う。Python を使う。
#
#   tools/testsrc/rtmp_empty_publish_test.sh
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tools/testsrc/dev_env.sh"
BIN="$ROOT/out/build/bin/Release"
OUT="$ROOT/out/check"
CFG="$OUT/rtmp3-config.json"
CTL="$BIN/denpachan-ctl.exe"
LOGS="$(cygpath -u "$LOCALAPPDATA")/denpachan/logs"
mkdir -p "$OUT"
rm -f "$CFG"
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1
taskkill //IM denpachan-rtmpd.exe //F >/dev/null 2>&1
sleep 1
SKIP_R=$(cat "$LOGS/rtmpd.log" 2>/dev/null | wc -l)
SKIP_C=$(cat "$LOGS/ch1.log" 2>/dev/null | wc -l)

"$BIN/denpachan-engine.exe" --config "$CFG" --log-file "$OUT/rtmp3-engine.log" >/dev/null 2>&1 &
sleep 2
"$CTL" set 1 '{"source":{"protocol":"rtmp","mode":"push","app":"live","key":"ch1key"}}' >/dev/null
"$CTL" start 1
sleep 3

python -I "$ROOT/tools/testsrc/rtmp_double_publish.py" 127.0.0.1 1935 live ch1key -
sleep 2
ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=1280x720:rate=30" -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
    -c:v libx264 -preset veryfast -tune zerolatency -b:v 2M -g 30 -bf 0 -pix_fmt yuv420p -c:a aac -b:a 160k -t 6 \
    -f flv "rtmp://127.0.0.1:1935/live/ch1key" >/dev/null 2>&1
"$CTL" stop 1 >/dev/null
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1

tail -n +"$((SKIP_C + 1))" "$LOGS/ch1.log" | grep -E "RTMP:|decoder:|playout: live" | cut -c12-
RESYNCS=$(tail -n +"$((SKIP_R + 1))" "$LOGS/rtmpd.log" | grep -c "resynced")
LIVE=$(tail -n +"$((SKIP_C + 1))" "$LOGS/ch1.log" | grep -c "playout: live")
echo "rtmpd resyncs: $RESYNCS, playout live: $LIVE"
# 読み始めに 1 回失敗して頼み直すことはありうるので、1 回までは許す
if [ "$RESYNCS" -le 1 ] && [ "$LIVE" -ge 1 ]; then echo "OK"; exit 0; else echo "FAIL"; exit 1; fi
