#!/usr/bin/env bash
# 映像だけが途切れて音声は届き続けるときの試験 (VB-Audio Virtual Cable を使う、A/V 同期オン)。
#
# worker の試験用オプション --test-drop-video で、受信して 8 秒後から 3 秒間、映像のパケットだけを捨てる
# (音声は届き続ける。送出側の ffmpeg では、映像が来ない間は音声も溜めてしまうので作れない)。
# 途絶とみなす時間 (2 秒) を超えて映像が途切れても、時間軸を取り直さず、音声の合わせ直し (engine の resyncs) が
# 増えないことを確かめる。試験用の設定ファイル (out/check/reanchor-config.json) を使う。
#
#   tools/testsrc/reanchor_test.sh
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tools/testsrc/dev_env.sh"
BIN="$ROOT/out/build/bin/Release"
OUT="$ROOT/out/check"
CTL="$BIN/denpachan-ctl.exe"
CFG="$OUT/reanchor-config.json"
CH_LOG="$(cygpath -u "$LOCALAPPDATA")/denpachan/logs/ch1.log"
mkdir -p "$OUT"
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1
sleep 1
rm -f "$CFG"
SKIP=$(cat "$CH_LOG" 2>/dev/null | wc -l)
resyncs() { "$CTL" state | grep -oE '"resyncs": [0-9]+' | head -1 | grep -oE '[0-9]+'; }

"$BIN/denpachan-engine.exe" --config "$CFG" --log-file "$OUT/reanchor-engine.log" >/dev/null 2>&1 &
sleep 2
"$CTL" set 1 '{"source":{"protocol":"srt","mode":"listener","port":9001},"audio":{"device":"CABLE Input","startChannel":1},"debugArgs":"--test-drop-video 8"}' >/dev/null
"$CTL" start 1
sleep 2

ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=1280x720:rate=60" -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
    -c:v libx264 -preset veryfast -tune zerolatency -b:v 4M -g 60 -bf 0 -pix_fmt yuv420p -c:a aac -b:a 192k -t 20 \
    -f mpegts "srt://127.0.0.1:9001?mode=caller&latency=120000&pkt_size=1316" >/dev/null 2>&1 &
SENDER=$!
sleep 7
BEFORE=$(resyncs)
wait "$SENDER"
AFTER=$(resyncs)
"$CTL" stop 1 >/dev/null
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1

tail -n +"$((SKIP + 1))" "$CH_LOG" | grep -E "playout:" | cut -c12-
echo "audio resyncs: before the gap $BEFORE, after $AFTER"
if [ -n "$BEFORE" ] && [ "$BEFORE" = "$AFTER" ] && ! tail -n +"$((SKIP + 1))" "$CH_LOG" | grep -q "re-anchor"; then
    echo "OK   映像が途切れても時間軸を取り直さず、音声は合わせ直さない"
    exit 0
fi
echo "FAIL 映像の途切れで時間軸を取り直した / 音声を合わせ直した"
exit 1
