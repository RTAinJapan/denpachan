#!/usr/bin/env bash
# 映像だけが途切れて音声は届き続けるときの試験 (VB-Audio Virtual Cable を使う、A/V 同期オン)。
#
# worker の試験用オプション --test-drop-video で、受信して 16 秒後から 3 秒間、映像のパケットだけを捨てる
# (鳴らし始めて 10 秒の間は、ずれを一度に詰めて合わせ直すことがあるので、その後にする)
# (音声は届き続ける。送出側の ffmpeg では、映像が来ない間は音声も溜めてしまうので作れない)。
# 途絶とみなす時間 (2 秒) を超えて映像が途切れても、時間軸を取り直さず、その途切れで音声を合わせ直さない
# (engine の resyncs が増えない) ことを確かめる。試験用の設定ファイル (out/check/reanchor-config.json) を使う。
#
# 合わせ直しは、映像の途切れと関係のない理由でも起きる (音声が揺れで 5 ms を超えて遅れて届いた、など)。回数だけを
# 見ると、ループバックの揺れでたまに落ちるので、worker の debug のログで、途切れの前の計測より後に時間軸を一度に
# 動かした理由を見て判定する:
#   - 時間軸の取り直し (re-anchor)、映像の遅れ (video arrived ... late)                    → 途切れのせい。失敗
#   - 途切れの間に遅らせていないのに、明けたあとで余りを一度に戻した (caught up で 5 ms 超)  → 途切れのせい。失敗
#   - 音声が揺れで 5 ms を超えて遅れて届いた (audio arrived ... late)                        → 途切れとは別。その回数は許す
#   - 明けてすぐ (途切れの続きの間) に音声が 5 ms を超えて遅れて届いた (... late (outage))   → 同上。遅らせた分をあとで
#     一度に戻すので、1 件につき 2 回まで許す
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
export SPDLOG_LEVEL="info,ch1=debug"  # 時間軸を動かした理由 (playout: moved ... (理由)) を出す
rm -f "$CFG"
SKIP=$(cat "$CH_LOG" 2>/dev/null | wc -l)
resyncs() { "$CTL" state | grep -oE '"resyncs": [0-9]+' | head -1 | grep -oE '[0-9]+'; }

"$BIN/denpachan-engine.exe" --config "$CFG" --log-file "$OUT/reanchor-engine.log" >/dev/null 2>&1 &
sleep 2
"$CTL" set 1 '{"source":{"protocol":"srt","mode":"listener","port":9001},"audio":{"device":"CABLE Input","startChannel":1},"debugArgs":"--test-drop-video 16"}' >/dev/null
"$CTL" start 1
sleep 2

ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=1280x720:rate=60" -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
    -c:v libx264 -preset veryfast -tune zerolatency -b:v 4M -g 60 -bf 0 -pix_fmt yuv420p -c:a aac -b:a 192k -t 28 \
    -f mpegts "srt://127.0.0.1:9001?mode=caller&latency=120000&pkt_size=1316" >/dev/null 2>&1 &
SENDER=$!
sleep 15
BEFORE=$(resyncs)
T0=$(date "+%Y-%m-%d %H:%M:%S.%3N")
wait "$SENDER"
AFTER=$(resyncs)
LAST_RESYNC=$("$CTL" state | grep -oE '"lastResync": "[^"]*"' | head -1)
"$CTL" stop 1 >/dev/null
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1

RUN_LOG="$OUT/reanchor-ch1.log"
tail -n +"$((SKIP + 1))" "$CH_LOG" >"$RUN_LOG"
grep -E " (info|warning|error) playout:" "$RUN_LOG" | cut -c12-
# 途切れの前の計測 (T0) より後の行
after() { awk -v t0="$T0" '($1 " " $2) >= t0' "$RUN_LOG"; }
over5() { grep -oE "[0-9.]+" | awk '$1 > 5' | wc -l; }
REANCHOR=$(grep -c "re-anchor" "$RUN_LOG")
VIDEO_LATE=$(after | grep -c "playout: video arrived")
CAUGHT=$(after | grep -oE "delay [0-9.]+ ms more than needed; caught up" | grep -oE "^delay [0-9.]+" | over5)
JITTER=$(after | grep -oE "audio arrived [0-9.]+ ms late; delay increased" | grep -oE "arrived [0-9.]+" | over5)
TAIL_LATE=$(after | grep -oE "audio arrived [0-9.]+ ms late \((outage|repeated outages)\)" | grep -oE "arrived [0-9.]+" | over5)
ALLOWED=$((JITTER + 2 * TAIL_LATE))
echo "audio resyncs: before the gap $BEFORE, after $AFTER (allowed +$ALLOWED: audio late by jitter $JITTER, late right after the gap $TAIL_LATE)"
echo "after the measurement: re-anchor $REANCHOR, video late $VIDEO_LATE, caught up over 5 ms $CAUGHT"
echo "moves over 5 ms after the measurement:"
after | grep -E "playout: moved [-+][0-9.]+ ms" | awk '{ v = $7; sub(/^[-+]/, "", v); if (v + 0 > 5) print "  " substr($0, 12) }'
echo "last resync (engine): ${LAST_RESYNC:-?}"
fail=""
[ -z "$BEFORE" ] || [ -z "$AFTER" ] && fail="$fail / 合わせ直しの回数が取れなかった"
[ "$REANCHOR" -gt 0 ] && fail="$fail / 時間軸を取り直した"
[ "$VIDEO_LATE" -gt 0 ] && fail="$fail / 映像が遅れて届いたとして遅らせた"
[ "$CAUGHT" -gt "$TAIL_LATE" ] && fail="$fail / 途切れで遅らせていないのに、余りを一度に戻した"
[ -n "$BEFORE" ] && [ -n "$AFTER" ] && [ "$AFTER" -gt $((BEFORE + ALLOWED)) ] && fail="$fail / 理由の分からない合わせ直しがある"
if [ -z "$fail" ]; then
    echo "OK   映像が途切れても時間軸を取り直さず、その途切れで音声を合わせ直さない"
    exit 0
fi
echo "FAIL${fail# /}"
exit 1
