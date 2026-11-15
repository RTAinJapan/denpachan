#!/usr/bin/env bash
# 録画ファイルの中の位置 (受信口の record.position) の試験 (Python の websockets を使う)。
#
# 受信口 (WebSocket) の channels を 1 秒ごとに書き留めながら録画し、ファイルが閉じたあとで、最後に出ていた position と、
# 録画ファイルの最後のパケットの時刻 (ffprobe) を比べる (record_position_test.py)。
#   1. パススルー: 8 秒受信 → 3 秒切れて再接続 (同じファイルに続ける) → 8 秒受信。途切れた時間はファイルに入らないので、
#      seconds (録画を始めてからの実時間) はファイルより先へ進む。position はファイルと合う
#   2. 途切れでファイルが閉じたあとの受信 → 新しいファイル。position は 0 から数え直す
#   3. MPEG-TS のパススルー
#   4. 同じ接続のままタイムスタンプが 8 秒戻る (worker の試験用オプション --test-ts-rewind)
#   5. 再エンコード (GPU) で 1 と同じ
# 試験用の設定ファイル (out/check/recpos-config.json) を使うので、普段の設定は触らない。録画は out/check/recpos-rec に書く。
# 受信口は 9109 番で開く (普段の 9100 番に繋ぎに来る bot と混ざらないように)。
#
#   tools/testsrc/record_position_test.sh [encoder]    encoder: nvenc (既定) / qsv / auto / none (5 を省く)
set -u

ENCODER="${1:-nvenc}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tools/testsrc/dev_env.sh"
BIN="$ROOT/out/build/bin/Release"
OUT="$ROOT/out/check"
CFG="$OUT/recpos-config.json"
LOG="$OUT/recpos-engine.log"
REC="$OUT/recpos-rec"
SAMPLES="$OUT/recpos-samples.jsonl"
STOP="$OUT/recpos-stop"
CTL="$BIN/denpachan-ctl.exe"
PY="$ROOT/tools/testsrc/record_position_test.py"
PORT=9109
GAP=6                # 映像がこの秒数来なければファイルを区切る。送出の ffmpeg の起動と SRT の接続にかかる時間より十分長くする
AFTER=$((GAP + 2))   # 送出が終わってからファイルが閉じるまで待つ時間
mkdir -p "$OUT"
if ! python -E -c "import websockets" 2>/dev/null; then
    echo "FAIL: Python の websockets がありません (pip install websockets)"
    exit 1
fi
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1
sleep 1
rm -rf "$CFG" "$LOG" "$REC" "$SAMPLES" "$STOP"

send() {
    ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=1280x720:rate=60" \
        -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
        -c:v libx264 -preset veryfast -tune zerolatency -b:v 4M -g 60 -bf 0 -pix_fmt yuv420p \
        -c:a aac -b:a 192k -pes_payload_size 0 -muxdelay 0 -muxpreload 0 -t "$1" \
        -f mpegts "srt://127.0.0.1:9001?mode=caller&latency=120000&pkt_size=1316" >/dev/null 2>&1
}
record() { "$CTL" set 1 "{\"record\":$1}" >/dev/null; }
summary() { "$CTL" summary | head -1 | grep -oE "rec [^|]*"; }

"$BIN/denpachan-engine.exe" --config "$CFG" --log-file "$LOG" >/dev/null 2>&1 &
sleep 2
"$CTL" set-general "{\"metrics\":{\"enabled\":true,\"bind\":\"127.0.0.1\",\"port\":$PORT}}" >/dev/null
record "{\"enabled\":true,\"dir\":\"$(cygpath -w "$REC" | sed 's/\\/\\\\/g')\",\"template\":\"{ch} {hh}{mm}{ss}\",\"gapTimeoutSec\":$GAP,\"mode\":\"passthrough\",\"container\":\"mkv\"}"
"$CTL" start 1
sleep 2
python -E "$PY" log "$PORT" "$SAMPLES" "$STOP" &
LOGGER=$!
LABELS=()

echo "==== 1. パススルー: 8 秒 → 3 秒切れて再接続 → 8 秒"
send 8
sleep 3
send 8
sleep 1
summary
sleep "$AFTER"
LABELS+=("gap:パススルー、途切れての再接続をまたぐ")

echo "==== 2. ファイルが閉じたあとの受信 → 新しいファイル"
send 6
sleep "$AFTER"
LABELS+=("plain:新しいファイル")

echo "==== 3. MPEG-TS のパススルー"
record '{"container":"ts"}'
send 6
sleep "$AFTER"
record '{"container":"mkv"}'
LABELS+=("plain:MPEG-TS のパススルー")

echo "==== 4. 同じ接続のままタイムスタンプが 8 秒戻る (受信して 6 秒後)"
"$CTL" set 1 '{"debugArgs":"--test-ts-rewind 6"}' >/dev/null
sleep 3
send 14
sleep 1
summary
sleep "$AFTER"
"$CTL" set 1 '{"debugArgs":""}' >/dev/null
sleep 3
LABELS+=("plain:タイムスタンプの巻き戻り")

if [ "$ENCODER" != "none" ]; then
    echo "==== 5. 再エンコード ($ENCODER): 8 秒 → 3 秒切れて再接続 → 8 秒"
    record "{\"mode\":\"encode\",\"encode\":{\"video\":{\"codec\":\"h264\",\"encoder\":\"$ENCODER\"}}}"
    send 8
    sleep 3
    send 8
    sleep 1
    summary
    sleep "$AFTER"
    LABELS+=("gap:再エンコード ($ENCODER)、途切れての再接続をまたぐ")
fi

touch "$STOP"
wait "$LOGGER"
"$CTL" stop 1
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
rm -f "$STOP"

echo
echo "==== 結果 (書き留めた値: ${SAMPLES#$ROOT/})"
python -E "$PY" check "$SAMPLES" "${LABELS[@]}"
FAIL=$?
echo
echo "==== worker log (recorder, this run)"
grep -E "test: timestamps|record:" "$(cygpath -u "$LOCALAPPDATA")/denpachan/logs/ch1.log" | tail -24 | cut -c12-
exit $FAIL
