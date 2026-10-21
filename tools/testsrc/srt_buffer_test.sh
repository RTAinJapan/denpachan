#!/usr/bin/env bash
# SRT の受信バッファの試験: latency を大きくして高いビットレートを受けても取りこぼさないこと。
#
# worker を latency 3000 ms で待ち受けにし、1080p60 を 50 Mbps で 20 秒送る。
# 受信側は latency の間に届く量 (約 3 秒 × 50 Mbps ≒ 19 MB) を溜めておく必要がある。libsrt の既定の受信バッファ
# (約 12 MB) では約 1.7 秒分で頭打ちになり (フロー制御で送出側が待たされる)、設定した latency を使い切れない。
# worker のログの SRT の統計 (drop、溜まっている時間 buf) と、受信バッファの大きさのログを見る。
#
#   tools/testsrc/srt_buffer_test.sh
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tools/testsrc/dev_env.sh"
BIN="$ROOT/out/build/bin/Release"
OUT="$ROOT/out/check"
LOG="$OUT/srt_buffer_worker.log"
mkdir -p "$OUT"
taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1
rm -f "$LOG"

"$BIN/denpachan-worker.exe" --ch 1 --cam 0 --port 9001 --latency 3000 --no-audio --log-file "$LOG" >/dev/null 2>&1 &
sleep 1
ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=1920x1080:rate=60,noise=alls=40:allf=t" \
    -c:v libx264 -preset ultrafast -tune zerolatency -b:v 50M -minrate 50M -maxrate 50M -bufsize 10M -g 120 -bf 0 -pix_fmt yuv420p \
    -t 20 -f mpegts "srt://127.0.0.1:9001?mode=caller&latency=3000000&pkt_size=1316" >/dev/null 2>&1
sleep 5
taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1

grep -E "receive buffer" "$LOG" | cut -c12-
grep -oE "[0-9.]+ Mbps" "$LOG" | sort -t' ' -k1,1g | tail -1 | sed 's/^/max bitrate: /'
DROP=$(grep -oE "drop [0-9]+" "$LOG" | tail -1 | grep -oE "[0-9]+")
echo "SRT drop (total): ${DROP:-?}"
grep -oE "lost [0-9]+ retrans [0-9]+" "$LOG" | tail -1 | sed 's/^/SRT (total): /'
grep -oE "buf [0-9]+ ms" "$LOG" | sort -t' ' -k2,2n | tail -1 | sed 's/^/max receive buffer used: /'
BUF=$(grep -oE "buf [0-9]+ ms" "$LOG" | awk '{print $2}' | sort -n | tail -1)
if [ "${DROP:-1}" = 0 ] && [ "${BUF:-0}" -ge 2700 ]; then echo "OK   取りこぼしなし、latency 分 (3 秒近く) を溜められる"; exit 0; fi
echo "FAIL 取りこぼした / latency 分を溜められない (buf ${BUF:-?} ms)"
exit 1
