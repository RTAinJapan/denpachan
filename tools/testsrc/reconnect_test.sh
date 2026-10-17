#!/usr/bin/env bash
# 再接続 / 無信号の試験。
#
# worker を起動し、送出を ON (10 秒) → OFF (5 秒) を繰り返す。その間 DirectShow で Cam を読み続ける
# (読み手がいないと worker は書き込みを省くため)。最後に worker のログから要点を出す。
#
#   tools/testsrc/reconnect_test.sh [回数]
#
# 事前に ./build.ps1 でビルドし、vcam を登録しておくこと。
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/out/build/bin/Release"
OUT="$ROOT/out/check"
CYCLES="${1:-3}"
PORT=9001
BUFFER="${BUFFER:-0}"
mkdir -p "$OUT"
LOG="$OUT/reconnect_worker.log"
rm -f "$LOG"

"$BIN/denpachan-worker.exe" --ch 1 --cam 1 --port "$PORT" --buffer "$BUFFER" --log-file "$LOG" >/dev/null 2>&1 &
WORKER=$!
sleep 1

# 読み手 (OBS の代わり)
ffmpeg -hide_banner -loglevel error -f dshow -video_size 1920x1080 -framerate 60 -pixel_format nv12 \
    -i video="denpachan Cam 1" -f null - >/dev/null 2>&1 &
READER=$!

send() {
    ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=1920x1080:rate=60" \
        -c:v libx264 -preset veryfast -tune zerolatency -b:v 8M -g 120 -bf 0 -pix_fmt yuv420p \
        -t "$1" -f mpegts "srt://127.0.0.1:$PORT?mode=caller&latency=200000&pkt_size=1316" >/dev/null 2>&1
}

for i in $(seq 1 "$CYCLES"); do
    echo "cycle $i: send 10s"
    send 10
    echo "cycle $i: pause 5s"
    sleep 5
done

kill "$READER" 2>/dev/null
taskkill //PID "$(cat /proc/$WORKER/winpid 2>/dev/null || echo 0)" //F >/dev/null 2>&1 || kill "$WORKER" 2>/dev/null
wait 2>/dev/null

echo "---- events"
grep -E "connected|disconnected|live|no signal|re-anchor|error|warn" "$LOG" | cut -c12-
echo "---- delay (ms) per second while live"
grep -oE "out [0-9]+ fps, delay [0-9]+ ms" "$LOG" | awk '$2 > 50 {print $5}' | sort -n | uniq -c | sort -k2 -n | tr '\n' ' '
echo
echo "---- final counters"
grep -oE "drop late [0-9]+ queue [0-9]+ \| err [0-9]+" "$LOG" | tail -1
echo "---- out fps while live"
grep -oE "out [0-9]+ fps" "$LOG" | awk '$2 > 50 {print $2}' | sort -n | uniq -c | tr "\n" " "; echo
