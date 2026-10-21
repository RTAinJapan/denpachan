#!/usr/bin/env bash
# 低遅延モード (A/V 同期オフ) で、送出側の時計がこちらより速いときの試験。
#
# 先にエンコードしたファイルを、タイムスタンプより 150 ppm 速く送る (-readrate 1.00015)。
# 送出側の時計が速いのと同じで、音声は少しずつ早く届くようになる。worker は余った遅延を少しずつ詰めて追うので、
# 音声の遅延 (到着から渡すまで) が増え続けないことを確かめる (追いつけないと増え続け、50 ms を超えるたびに一度に詰める)。
#
#   tools/testsrc/clock_drift_test.sh [送る秒 (既定 140)]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tools/testsrc/dev_env.sh"
BIN="$ROOT/out/build/bin/Release"
OUT="$ROOT/out/check"
LOG="$OUT/clock_drift_worker.log"
SEC=${1:-140}
SRC="$OUT/clock_drift_src.ts"
mkdir -p "$OUT"
taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1
rm -f "$LOG"
if [ ! -f "$SRC" ]; then
    ffmpeg -hide_banner -loglevel error -f lavfi -i "testsrc2=size=1280x720:rate=30" -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
        -c:v libx264 -preset ultrafast -b:v 2M -g 60 -bf 0 -pix_fmt yuv420p -c:a aac -b:a 128k -t 200 "$SRC"
fi

"$BIN/denpachan-worker.exe" --ch 1 --cam 0 --port 9001 --no-av-sync --log-file "$LOG" >/dev/null 2>&1 &
sleep 1
ffmpeg -hide_banner -loglevel error -readrate 1.00015 -i "$SRC" -t "$SEC" -c copy \
    -f mpegts "srt://127.0.0.1:9001?mode=caller&latency=120000&pkt_size=1316" >/dev/null 2>&1
taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1

# 1 秒ごとのログの音声の遅延 (最初の 20 秒は落ち着くまでなので除く)
grep -oE "gaps [0-9]+ delay [0-9]+ ms" "$LOG" | awk '{print $4}' | tail -n +20 >"$OUT/clock_drift_delay.txt"
N=$(wc -l <"$OUT/clock_drift_delay.txt")
FIRST=$(head -10 "$OUT/clock_drift_delay.txt" | awk '{ s += $1 } END { printf "%.1f", s / NR }')
LAST=$(tail -10 "$OUT/clock_drift_delay.txt" | awk '{ s += $1 } END { printf "%.1f", s / NR }')
# 遅れて届いたときに増やした分 (届き方の揺れ。時計のずれとは別) を差し引いて、時計のずれで増えた分を見る。
# 最初の 20 秒 (落ち着くまで) の分は数えない
START=$(grep -oE "^[0-9-]+ [0-9:.]+" "$LOG" | head -1 | cut -c12-19)
LATE=$(grep -E "audio: arrived [0-9.]+ ms late" "$LOG" | awk -v s="$START" '
    { split(substr($2, 1, 8), t, ":"); sec = t[1] * 3600 + t[2] * 60 + t[3]
      if (!base) { split(s, b, ":"); base = b[1] * 3600 + b[2] * 60 + b[3] }
      if (sec - base >= 20) { for (i = 1; i <= NF; i++) if ($i == "arrived") sum += $(i + 1) } }
    END { printf "%.1f", sum }')
NET=$(awk -v a="$FIRST" -v b="$LAST" -v l="$LATE" 'BEGIN { printf "%.1f", b - a - l }')
echo "audio delay: first 10 s avg $FIRST ms, last 10 s avg $LAST ms ($N samples), raised for late arrivals $LATE ms, drift $NET ms"
awk -v n="$NET" 'BEGIN { exit !(n < 3) }'
if [ $? = 0 ]; then echo "OK   時計のずれで遅延が増え続けない"; exit 0; fi
echo "FAIL 時計のずれで遅延が増え続けた"
exit 1
