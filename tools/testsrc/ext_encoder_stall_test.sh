#!/usr/bin/env bash
# CPU 録画 (外部 ffmpeg) で、ffmpeg が入力を読まなくなったときの試験。
#
# CPU 録画中に外部 ffmpeg を 25 秒止める (suspend.ps1)。worker が 10 秒で ffmpeg を止め、録画スレッドが書き込みで
# 止まったままにならず、新しいファイルで録画を続けることを確かめる。試験用の設定ファイル (out/check/ext-config.json) を使う。
#
#   tools/testsrc/ext_encoder_stall_test.sh
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tools/testsrc/dev_env.sh"
BIN="$ROOT/out/build/bin/Release"
OUT="$ROOT/out/check"
CFG="$OUT/ext-config.json"
REC="$OUT/ext-rec"
CTL="$BIN/denpachan-ctl.exe"
CH_LOG="$(cygpath -u "$LOCALAPPDATA")/denpachan/logs/ch1.log"
mkdir -p "$OUT"
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1
sleep 1
rm -rf "$CFG" "$REC"
SKIP=$(cat "$CH_LOG" 2>/dev/null | wc -l)
FAIL=0
check() {  # check <説明> <0 = OK>
    if [ "$2" = 0 ]; then echo "OK   $1"; else echo "FAIL $1"; FAIL=1; fi
}

"$BIN/denpachan-engine.exe" --config "$CFG" --log-file "$OUT/ext-engine.log" >/dev/null 2>&1 &
sleep 2
"$CTL" set 1 "{\"vcam\":0,\"record\":{\"enabled\":true,\"mode\":\"encode\",\"container\":\"mkv\",\"dir\":\"$(cygpath -w "$REC" | sed 's/\\/\\\\/g')\",\"template\":\"{ch} {hh}{mm}{ss}\",\"gapTimeoutSec\":5,\"encode\":{\"video\":{\"codec\":\"h264\",\"encoder\":\"cpu\"}}}}" >/dev/null
"$CTL" start 1
sleep 2

ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=1280x720:rate=60" -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
    -c:v libx264 -preset veryfast -tune zerolatency -b:v 4M -g 60 -bf 0 -pix_fmt yuv420p -c:a aac -b:a 192k -t 45 \
    -f mpegts "srt://127.0.0.1:9001?mode=caller&latency=120000&pkt_size=1316" >/dev/null 2>&1 &
SENDER=$!
sleep 8
EXT_PID=$(tail -n +"$((SKIP + 1))" "$CH_LOG" | grep -oE "external ffmpeg \(pid [0-9]+\)" | tail -1 | grep -oE "[0-9]+")
echo "pause the recording ffmpeg (pid $EXT_PID) for 25 s at $(date +%H:%M:%S)"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$ROOT/tools/testsrc/suspend.ps1" -ProcessId "$EXT_PID" -Milliseconds 25000 &
PAUSER=$!
wait "$SENDER"
wait "$PAUSER"
sleep 7
"$CTL" stop 1 >/dev/null
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1

LOG=$(tail -n +"$((SKIP + 1))" "$CH_LOG")
grep -E "record:" <<<"$LOG" | cut -c12-
grep -q "has not read its input" <<<"$LOG"
check "読まなくなった ffmpeg を止めた" $?
# 止めたあと (ffmpeg を止めていた 25 秒の間) に、新しい ffmpeg で録画を始め直している
awk '/has not read its input/ { seen = 1 } seen && /record: started/ { found = 1 } END { exit !found }' <<<"$LOG"
check "止めたあと、新しいファイルで録画を続けた" $?
exit $FAIL
