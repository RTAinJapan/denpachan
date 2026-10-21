#!/usr/bin/env bash
# 短い途切れのあとのまとめ届きの試験 (VB-Audio Virtual Cable を使う、A/V 同期オン)。
#
# 送出中に送出側の ffmpeg を <途切れの ms> だけ止める。-readrate_catchup を大きくしているので、再開すると
# 止まっていた分をすぐにまとめて送る (TCP の再送待ちや、送出側がまとめて送るときと同じ届き方)。
# 途絶とみなす時間 (2 秒) より短いので時間軸は取り直さず、遅れて届いた分だけ遅延が増える。
# 増えた遅延が数秒で元に戻り、出力の fps が戻ることを確かめる。
#
#   tools/testsrc/burst_test.sh [途切れの ms (既定 1000)]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tools/testsrc/dev_env.sh"
BIN="$ROOT/out/build/bin/Release"
OUT="$ROOT/out/check"
CTL="$BIN/denpachan-ctl.exe"
CFG="$OUT/burst-config.json"
ENGINE_LOG="$OUT/burst_engine.log"
WORKER_LOG="$(cygpath -u "$LOCALAPPDATA")/denpachan/logs/ch1.log"
PORT=9001
PAUSE_MS=${1:-1000}
mkdir -p "$OUT"
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1
sleep 1
rm -f "$CFG" "$ENGINE_LOG"
SKIP=$(cat "$WORKER_LOG" 2>/dev/null | wc -l)

"$BIN/denpachan-engine.exe" --config "$CFG" --log-file "$ENGINE_LOG" >/dev/null 2>&1 &
sleep 2
"$CTL" set 1 "{\"name\":\"burst\",\"source\":{\"protocol\":\"srt\",\"mode\":\"listener\",\"port\":$PORT},\"audio\":{\"device\":\"CABLE Input\",\"startChannel\":1}}" >/dev/null
"$CTL" start 1
sleep 2

# 先にファイルへエンコードしておき、それを送る (生成しながらだと、止めたときにタイムスタンプが飛ぶ)
SRC="$OUT/burst_src.ts"
if [ ! -f "$SRC" ]; then
    ffmpeg -hide_banner -loglevel error -f lavfi -i "testsrc2=size=1280x720:rate=60" -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
        -c:v libx264 -preset veryfast -tune zerolatency -b:v 4M -g 120 -bf 0 -pix_fmt yuv420p -c:a aac -b:a 192k -t 30 "$SRC"
fi
ffmpeg -hide_banner -loglevel error -readrate 1 -readrate_catchup 50 -i "$SRC" -c copy \
    -f mpegts "srt://127.0.0.1:$PORT?mode=caller&latency=120000&pkt_size=1316" >/dev/null 2>&1 &
SENDER=$!
sleep 10
echo "pause sender ${PAUSE_MS} ms at $(date +%H:%M:%S)"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$ROOT/tools/testsrc/suspend.ps1" -ProcessId "$(cat /proc/$SENDER/winpid)" -Milliseconds "$PAUSE_MS"
wait "$SENDER"

"$CTL" stop 1 >/dev/null
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
sleep 0.5

tail -n +"$((SKIP + 1))" "$WORKER_LOG" >"$OUT/burst_worker.log"
echo "==== worker (playout)"
grep -E "playout:" "$OUT/burst_worker.log" | cut -c12-
echo "==== worker (1 秒ごと: 出力 fps、映像の遅延、捨てたフレーム)"
grep -E "out [0-9]+ fps" "$OUT/burst_worker.log" | cut -c12-19 >"$OUT/burst_times.txt"
grep -oE "out [0-9]+ fps, delay [0-9]+ ms \| drop late [0-9]+ queue [0-9]+" "$OUT/burst_worker.log" | paste "$OUT/burst_times.txt" -
echo "==== engine (音声)"
grep -E "audio \[" "$ENGINE_LOG" | cut -c12-
