#!/usr/bin/env bash
# 1 つの RTMP 接続で 2 つのキーに publish したときの試験 (rtmpd が切れた接続を指したまま残さないこと)。
#
# ch1 (key ch1key) と ch2 (key ch2key) を RTMP push にし、rtmp_double_publish.py で 1 つの接続から
# ch1key → ch2key と publish して切る。そのあと FFmpeg で ch1key に publish し、
# - rtmpd のログに「前の publish を置き換えた」が出ない (前の接続は切れているので、置き換える相手はいないはず)
# - rtmpd が落ちていない (pid が変わらない)
# を確かめる。試験用の設定ファイル (out/check/rtmp2-config.json) を使う。
#
#   tools/testsrc/rtmp_double_publish_test.sh
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tools/testsrc/dev_env.sh"
BIN="$ROOT/out/build/bin/Release"
OUT="$ROOT/out/check"
CFG="$OUT/rtmp2-config.json"
LOG="$OUT/rtmp2-engine.log"
CTL="$BIN/denpachan-ctl.exe"
RTMPD_LOG="$(cygpath -u "$LOCALAPPDATA")/denpachan/logs/rtmpd.log"
mkdir -p "$OUT"
rm -f "$CFG" "$LOG"
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1
taskkill //IM denpachan-rtmpd.exe //F >/dev/null 2>&1
sleep 1
SKIP=$(cat "$RTMPD_LOG" 2>/dev/null | wc -l)
FAIL=0
rtmpd_pid() { "$CTL" state | grep -A3 '"rtmpServer"' | grep -oE '"pid": [0-9]+' | grep -oE '[0-9]+'; }

"$BIN/denpachan-engine.exe" --config "$CFG" --log-file "$LOG" >/dev/null 2>&1 &
sleep 2
"$CTL" set 1 '{"source":{"protocol":"rtmp","mode":"push","app":"live","key":"ch1key"}}' >/dev/null
"$CTL" set 2 '{"source":{"protocol":"rtmp","mode":"push","app":"live","key":"ch2key"}}' >/dev/null
"$CTL" start 1
"$CTL" start 2
sleep 3
PID=$(rtmpd_pid)
echo "rtmpd pid $PID"

python -I "$ROOT/tools/testsrc/rtmp_double_publish.py" 127.0.0.1 1935 live ch1key ch2key
sleep 2

ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=1280x720:rate=30" -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
    -c:v libx264 -preset veryfast -tune zerolatency -b:v 2M -g 30 -bf 0 -pix_fmt yuv420p -c:a aac -b:a 160k -t 4 \
    -f flv "rtmp://127.0.0.1:1935/live/ch1key" >/dev/null 2>&1
sleep 2

tail -n +"$((SKIP + 1))" "$RTMPD_LOG" >"$OUT/rtmp2-rtmpd.log"
echo "---- rtmpd log"
cut -c12- "$OUT/rtmp2-rtmpd.log"

if grep -q "replaces" "$OUT/rtmp2-rtmpd.log"; then echo "FAIL 切れた接続を置き換えた (前の publish が残っていた)"; FAIL=1; else echo "OK   置き換えは起きない"; fi
NOW=$(rtmpd_pid)
if [ -n "$PID" ] && [ "$NOW" = "$PID" ]; then echo "OK   rtmpd は動き続けている (pid $PID)"; else echo "FAIL rtmpd が起動し直した ($PID → $NOW)"; FAIL=1; fi

"$CTL" stop 1 >/dev/null
"$CTL" stop 2 >/dev/null
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
exit $FAIL
