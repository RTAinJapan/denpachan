#!/usr/bin/env bash
# 音声出力の試験 (VB-Audio Virtual Cable を使う)。
#
# engine → "CABLE Input"、audio_probe ← "CABLE Output" で録音し、
# 送出を ON (6 秒) → OFF (3 秒) → ON (6 秒) と切り替えながら次を確かめる。
# - 音切れ・クリック (不連続) がないこと
# - 途絶・再開の境目がフェードになっていること
# - 再接続の前後で音声の遅延が変わらないこと (engine のログ)
#
#   tools/testsrc/audio_test.sh [shared|exclusive] [default|lowmux]
#     lowmux: 送出側で音声をまとめずに多重化する (-pes_payload_size 0 -muxdelay 0)
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/out/build/bin/Release"
OUT="$ROOT/out/check"
PORT=9001
MODE="${1:-shared}"
MUX_ARGS=()
if [ "${2:-default}" = "lowmux" ]; then MUX_ARGS=(-pes_payload_size 0 -muxdelay 0 -muxpreload 0); fi
mkdir -p "$OUT"
ENGINE_LOG="$OUT/audio_engine.log"
WORKER_LOG="$OUT/audio_worker.log"
rm -f "$ENGINE_LOG" "$WORKER_LOG"

"$BIN/denpachan-engine.exe" --out "ch=1,dev=CABLE Input,start=1,mode=$MODE" --log-file "$ENGINE_LOG" >/dev/null 2>&1 &
"$BIN/denpachan-worker.exe" --ch 1 --cam 1 --port "$PORT" --log-file "$WORKER_LOG" >/dev/null 2>&1 &
sleep 1

"$BIN/audio_probe.exe" --dev "CABLE Output" --seconds 20 --wav "$OUT/audio_capture.wav" > "$OUT/audio_probe.txt" 2>&1 &
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
sleep 3
send 6
wait "$PROBE"

taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
sleep 0.5

echo "==== probe (CABLE Output)"
cat "$OUT/audio_probe.txt"
echo "==== engine"
grep -E "opened|lost|error|warn" "$ENGINE_LOG" | cut -c12-
grep -oE "latency [0-9.]+ ms err [-+0-9.]+ ms (slips|ratio) [-+0-9]+( ppm)? underruns [0-9]+ resyncs [0-9]+" "$ENGINE_LOG" |
    awk '{print "  " $0}' | uniq -c | tail -25
echo "==== worker (audio)"
grep -E "late|audio" "$WORKER_LOG" | grep -vE "Mbps" | cut -c12- | head -10
grep -oE "gaps [0-9]+ delay [0-9]+ ms a/v [-+0-9]+ ms" "$WORKER_LOG" | uniq -c | tail -15
