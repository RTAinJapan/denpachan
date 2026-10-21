#!/usr/bin/env bash
# RTMP (push / pull) の試験。FFmpeg を送出側に使う。
#
# 試験用の設定ファイル (out/check/rtmp-config.json) を使うので、普段の設定は触らない。
#
#   tools/testsrc/rtmp_test.sh
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tools/testsrc/dev_env.sh"
BIN="$ROOT/out/build/bin/Release"
OUT="$ROOT/out/check"
CFG="$OUT/rtmp-config.json"
LOG="$OUT/rtmp-engine.log"
CTL="$BIN/denpachan-ctl.exe"
mkdir -p "$OUT"
rm -f "$CFG" "$LOG" "$OUT"/rtmp-*.txt
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1
taskkill //IM denpachan-rtmpd.exe //F >/dev/null 2>&1

step() { echo; echo "==== $*"; }
line() { "$CTL" summary | sed -n "${1}p"; }
pid_of() { line "$1" | grep -oE "pid [0-9]+" | cut -d' ' -f2; }
rtmpd_state() { "$CTL" state | grep -A3 '"rtmpServer"' | grep -E '"pid"|"restarts"|"state"' | tr -d ' \n'; echo; }

# RTMP で送る (FFmpeg の RTMP クライアント)
publish() {  # publish <key> <seconds> <logname>
    ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=1280x720:rate=60" \
        -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
        -c:v libx264 -preset veryfast -tune zerolatency -b:v 4M -g 60 -bf 0 -pix_fmt yuv420p \
        -c:a aac -b:a 160k -t "$2" -f flv "rtmp://127.0.0.1:1935/live/$1" >> "$OUT/rtmp-$3.txt" 2>&1
}
# 切れたら繋ぎ直し続ける送出 (rtmpd の再起動をまたぐため)
publish_loop() { while true; do publish "$1" 600 "$2"; sleep 0.5; done; }

yavg() {
    ffmpeg -hide_banner -f dshow -video_size 1280x720 -framerate 60 -pixel_format nv12 -i video="denpachan Cam $1" \
        -frames:v 1 -vf signalstats,metadata=print:key=lavfi.signalstats.YAVG -f null - 2>&1 |
        grep -oE "YAVG=[0-9.]+" | head -1
}

"$BIN/denpachan-engine.exe" --config "$CFG" --log-file "$LOG" >/dev/null 2>&1 &
sleep 2
"$BIN/denpachan-worker.exe" --reset-cam 1 2>/dev/null
"$BIN/denpachan-worker.exe" --reset-cam 2 2>/dev/null

step "1. ch1 を RTMP push (app live / key ch1key) にして開始 → rtmpd が起動する"
"$CTL" set 1 '{"source":{"protocol":"rtmp","mode":"push","app":"live","key":"ch1key"},"audio":{"device":"CABLE Input"}}' | grep -E '"protocol"|"key"'
"$CTL" start 1
sleep 2
rtmpd_state

step "2. FFmpeg から push → 受信できる"
publish_loop ch1key a &
LOOP_A=$!
sleep 7
line 1
echo "cam1 $(yavg 1)"

step "3. 違うキーの publish は拒否される"
publish wrongkey 3 wrong
grep -iE "error|fail|refused|reset|closed" "$OUT/rtmp-wrong.txt" | head -2
line 1

step "4. 同じキーで 2 本目の publish → 今の送出元が送っているので断られる (映像は途切れない)"
publish ch1key 8 b &
sleep 5
line 1
wait $!
sleep 3

step "5. worker を強制終了 → 送出側は繋いだまま、次のキーフレームから復帰する"
PID=$(pid_of 1)
taskkill //PID "$PID" //F >/dev/null
sleep 5
line 1
echo "cam1 $(yavg 1)"

step "6. rtmpd を強制終了 → engine が再起動し、送出側が繋ぎ直す"
taskkill //IM denpachan-rtmpd.exe //F >/dev/null
sleep 8
rtmpd_state
line 1

step "7. RTMP pull: ch2 が FFmpeg の RTMP サーバ (1936) から取りに行く"
( while true; do ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=1280x720:rate=30" \
      -c:v libx264 -preset veryfast -tune zerolatency -b:v 3M -g 30 -bf 0 -pix_fmt yuv420p \
      -f flv -listen 1 "rtmp://127.0.0.1:1936/live/pullkey" >> "$OUT/rtmp-pullserver.txt" 2>&1; sleep 0.3; done ) &
PULL_SERVER=$!
"$CTL" set 2 '{"source":{"protocol":"rtmp","mode":"pull","url":"rtmp://127.0.0.1:1936/live/pullkey"},"vcam":2}' >/dev/null
"$CTL" start 2
sleep 9
line 2
echo "cam2 $(yavg 2)"

step "8. ch1 を停止 → push のチャンネルがなくなるので rtmpd も止まる"
"$CTL" stop 1
sleep 2
rtmpd_state
line 1

kill "$LOOP_A" "$PULL_SERVER" 2>/dev/null
taskkill //IM ffmpeg.exe //F >/dev/null 2>&1
"$CTL" stop 2 >/dev/null
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
echo
echo "==== engine log"
grep -E "warn|error|rtmpd|started|stopped" "$LOG" | cut -c12- | tail -20
