#!/usr/bin/env bash
# 実際のサーバーからの受信試験 (RTMP pull、または SRT caller)。
#
# 試験用の設定 (out/check/pull-config.json) で engine を起動し、ch1 を RTMP pull / SRT caller にして受信する。
# URL が srt://ホスト:ポート なら SRT caller (latency 120 ms、パスフレーズなし)、それ以外は RTMP pull。
# 状態を 1 秒ごとに表示し、仮想カメラ (DirectShow) から映像が取れるか確かめ、画面を撮る。
# 音声は "CABLE Input" (VB-Audio Virtual Cable) に出す。
#
#   tools/testsrc/pull_test.sh <url> [seconds] [playout_buffer_ms]
#     例: tools/testsrc/pull_test.sh rtmp://example.com/app/key 30 60
#         tools/testsrc/pull_test.sh srt://example.com:5000 30
set -u

URL="${1:?usage: pull_test.sh <rtmp-url> [seconds] [playout_buffer_ms]}"
SECONDS_TO_WATCH="${2:-30}"
BUFFER_MS="${3:-0}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tools/testsrc/dev_env.sh"
BIN="$ROOT/out/build/bin/Release"
OUT="$ROOT/out/check"
CFG="$OUT/pull-config.json"
LOG="$OUT/pull-engine.log"
CTL="$BIN/denpachan-ctl.exe"
mkdir -p "$OUT"
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1
sleep 1
rm -f "$CFG" "$LOG"

step() { echo; echo "==== $*"; }

"$BIN/denpachan-engine.exe" --config "$CFG" --log-file "$LOG" >/dev/null 2>&1 &
sleep 2

case "$URL" in
    srt://*)
        HOSTPORT="${URL#srt://}"
        HOSTPORT="${HOSTPORT%%[/?]*}"
        SOURCE="{\"protocol\":\"srt\",\"mode\":\"caller\",\"host\":\"${HOSTPORT%:*}\",\"port\":${HOSTPORT##*:},\"latencyMs\":120}"
        ;;
    *) SOURCE="{\"protocol\":\"rtmp\",\"mode\":\"pull\",\"url\":\"$URL\"}" ;;
esac
step "1. ch1 を $SOURCE で開始 (再生バッファ ${BUFFER_MS} ms)"
"$CTL" set 1 "{\"name\":\"pull test\",\"source\":$SOURCE,\"playoutBufferMs\":$BUFFER_MS,\"audio\":{\"device\":\"CABLE Input\",\"startChannel\":1}}" >/dev/null
"$CTL" start 1

step "2. ${SECONDS_TO_WATCH} 秒間の状態 (1 秒ごと)"
"$CTL" watch "$SECONDS_TO_WATCH"

step "3. 詳細"
"$CTL" state | sed -n '/"channels"/,/"id": 2/p' | grep -E '"(state|connected|codec|width|height|hwDecode|bitrateMbps|decodeFps|outputFps|delayMs|droppedLate|droppedQueue|decodeErrors|lastError|rttMs|retransBytes|avOffsetMs|gaps|latencyMs|slips|underruns|playing)"' | sed 's/^ *//' | tr '\n' ' '
echo

step "4. 仮想カメラ (DirectShow) から映像を取る"
YAVG=$(ffmpeg -hide_banner -f dshow -i video="denpachan Cam 1" -frames:v 30 -vf signalstats,metadata=print:key=lavfi.signalstats.YAVG -f null - 2>&1 |
    grep -o 'YAVG=[0-9.]*' | tail -1)
echo "cam1 $YAVG (slate は 32.9 付近)"

step "5. 画面を撮る (out/check/ui-pull.png)"
"$BIN/denpachan.exe" --screenshot "$OUT/ui-pull.png" --delay 6 --select 1

step "6. 停止"
"$CTL" stop 1
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1

echo
echo "==== worker log"
grep -vE "^\s*$" "$(cygpath -u "$LOCALAPPDATA")/denpachan/logs/ch1.log" 2>/dev/null | tail -15 | cut -c12-
