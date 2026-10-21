#!/usr/bin/env bash
# 音声出力の試験 (VB-Audio Virtual Cable を使う)。
#
# engine → "CABLE Input"、audio_probe ← "CABLE Output" で録音し、
# 送出を ON (6 秒) → OFF (3 秒) → ON (6 秒) と切り替えながら次を確かめる。
# - 音切れ・クリック (不連続) がないこと
# - 途絶・再開の境目がフェードになっていること
# - 再接続の前後で音声の遅延が変わらないこと (engine の状態)
# 試験用の設定ファイル (out/check/audio-config.json) を使うので、普段の設定は触らない。
#
#   tools/testsrc/audio_test.sh [shared|exclusive] [default|lowmux]
#     lowmux: 送出側で音声をまとめずに多重化する (-pes_payload_size 0 -muxdelay 0)
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tools/testsrc/dev_env.sh"
BIN="$ROOT/out/build/bin/Release"
OUT="$ROOT/out/check"
CTL="$BIN/denpachan-ctl.exe"
CFG="$OUT/audio-config.json"
ENGINE_LOG="$OUT/audio_engine.log"
WORKER_LOG="$(cygpath -u "$LOCALAPPDATA")/denpachan/logs/ch1.log"
PORT=9001
MODE="${1:-shared}"
MUX_ARGS=()
if [ "${2:-default}" = "lowmux" ]; then MUX_ARGS=(-pes_payload_size 0 -muxdelay 0 -muxpreload 0); fi
mkdir -p "$OUT"
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1
sleep 1
rm -f "$CFG" "$ENGINE_LOG"
SKIP=$(cat "$WORKER_LOG" 2>/dev/null | wc -l)

# engine の状態から ch1 の音声出力の値を 1 行で出す
audio_state() {
    # 1 行ずつ見る (deviceId の値に "}" が入るので、JSON を 1 行にして括弧で切り出すことはできない)
    "$CTL" state | grep -A25 '"audioOut": {' | head -26 |
        grep -oE '"(latencyMs|errorMs|slips|underruns|resyncs)": [-0-9.]+' | tr '\n' ' '
    echo
}

"$BIN/denpachan-engine.exe" --config "$CFG" --log-file "$ENGINE_LOG" >/dev/null 2>&1 &
sleep 2
"$CTL" set 1 "{\"source\":{\"protocol\":\"srt\",\"mode\":\"listener\",\"port\":$PORT},\"audio\":{\"device\":\"CABLE Input\",\"startChannel\":1,\"mode\":\"$MODE\"}}" >/dev/null
"$CTL" start 1
sleep 1

"$BIN/audio_probe.exe" --dev "CABLE Output" --seconds 20 --wav "$OUT/audio_capture.wav" >"$OUT/audio_probe.txt" 2>&1 &
PROBE=$!
sleep 1

send() {
    ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=1280x720:rate=60" \
        -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
        -c:v libx264 -preset veryfast -tune zerolatency -b:v 4M -g 120 -bf 0 -pix_fmt yuv420p \
        -c:a aac -b:a 192k -t "$1" "${MUX_ARGS[@]}" -f mpegts "srt://127.0.0.1:$PORT?mode=caller&latency=120000&pkt_size=1316" \
        >/dev/null 2>&1
}

send 6
echo "==== engine audio (1 回目の受信の終わり): $(audio_state)"
sleep 3
send 6
echo "==== engine audio (2 回目の受信の終わり): $(audio_state)"
wait "$PROBE"

"$CTL" stop 1 >/dev/null
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
sleep 0.5

echo "==== probe (CABLE Output)"
cat "$OUT/audio_probe.txt"
echo "==== engine"
grep -E "audio \[|opened|lost|error|warn" "$ENGINE_LOG" | cut -c12-
echo "==== worker (audio)"
tail -n +"$((SKIP + 1))" "$WORKER_LOG" | grep -E "late|audio" | grep -vE "Mbps" | cut -c12- | head -10
tail -n +"$((SKIP + 1))" "$WORKER_LOG" | grep -oE "gaps [0-9]+ delay [0-9]+ ms a/v [-+0-9]+ ms" | uniq -c | tail -15
