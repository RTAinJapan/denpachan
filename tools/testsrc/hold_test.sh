#!/usr/bin/env bash
# 途絶時の「最後のフレームを保持」と NO SIGNAL 表示の試験。
#
# 各場面で Cam 1 から 1 フレーム取り、平均輝度で判定する
# (NO SIGNAL スレートは暗いので YAVG < 50、テストパターンは明るい)。
#
#   tools/testsrc/hold_test.sh
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tools/testsrc/dev_env.sh"
BIN="$ROOT/out/build/bin/Release"
OUT="$ROOT/out/check"
PORT=9001
mkdir -p "$OUT"
LOG="$OUT/hold_worker.log"
rm -f "$LOG"
FAIL=0

# Cam 1 の 1 フレームの平均輝度
yavg() {
    ffmpeg -hide_banner -f dshow -video_size 1280x720 -framerate 60 -pixel_format nv12 -i video="denpachan Cam 1" \
        -frames:v 1 -vf signalstats,metadata=print:key=lavfi.signalstats.YAVG -f null - 2>&1 |
        grep -oE "YAVG=[0-9.]+" | head -1 | cut -d= -f2
}

expect() {  # expect <場面> <slate|picture>
    local y
    y=$(yavg)
    local got=picture
    if [ -z "$y" ]; then got="capture-failed"; elif awk "BEGIN{exit !($y < 50)}"; then got=slate; fi
    if [ "$got" = "$2" ]; then echo "OK   $1: $got (YAVG $y)"; else echo "FAIL $1: expected $2, got $got (YAVG $y)"; FAIL=1; fi
}

send() {
    ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=1920x1080:rate=60" \
        -c:v libx264 -preset veryfast -tune zerolatency -b:v 8M -g 120 -bf 0 -pix_fmt yuv420p \
        -t "$1" -f mpegts "srt://127.0.0.1:$PORT?mode=caller&latency=120000&pkt_size=1316" >/dev/null 2>&1
}

start_worker() {
    "$BIN/denpachan-worker.exe" --ch 1 --cam 1 --port "$PORT" --log-file "$LOG" >/dev/null 2>&1 &
    WORKER=$!
    sleep 1
}

stop_worker() {
    kill -9 "$WORKER" 2>/dev/null
    taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1
    wait "$WORKER" 2>/dev/null
}

"$BIN/denpachan-worker.exe" --reset-cam 1 2>/dev/null
expect "1. リセット直後 (worker なし)" slate

start_worker
expect "2. worker 起動、未受信" slate

# 読み手 (OBS の代わり) がいる状態で受信 → 途絶
ffmpeg -hide_banner -loglevel error -f dshow -video_size 1920x1080 -framerate 60 -pixel_format nv12 \
    -i video="denpachan Cam 1" -f null - >/dev/null 2>&1 &
READER=$!
send 6
sleep 4
expect "3. 送出停止から 4 秒後" picture

# OBS が開いたまま worker が落ちた場合。
# (共有メモリは開いているプロセスがなくなると消える。本番はエンジンが常に保持する)
stop_worker
expect "4. worker を強制終了した後" picture
kill "$READER" 2>/dev/null

"$BIN/denpachan-worker.exe" --reset-cam 1 2>/dev/null
expect "5. --reset-cam の後" slate

# 読み手がいない状態で受信 → 途絶 → 読み手が開く
start_worker
send 5
sleep 3
expect "6. 誰も見ていない間に受信して途絶、その後に開く" picture

stop_worker
echo "---- worker events"
grep -E "connected|disconnected|live|stalled|vcam" "$LOG" | cut -c12-
exit $FAIL
