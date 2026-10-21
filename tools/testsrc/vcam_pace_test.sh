#!/usr/bin/env bash
# 仮想カメラの出し方の試験: 出力の fps より遅い映像を受けているときに、同じ絵を余分に挟まないこと。
#
# Cam 1 を 60 fps で開き、30 fps と 60 fps の映像を送って、届いたサンプルのうち直前と同じ絵 (同じハッシュ) の
# 割合を見る。受信中は新しいフレームが届くまで待つので、どちらもほぼ 0 % になるはず
# (待ちが短いと、30 fps では毎回フレームが届く直前に同じ絵を 1 枚挟み、約 50 % になる)。
#
#   tools/testsrc/vcam_pace_test.sh
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tools/testsrc/dev_env.sh"
BIN="$ROOT/out/build/bin/Release"
OUT="$ROOT/out/check"
PORT=9001
mkdir -p "$OUT"
LOG="$OUT/vcam_pace_worker.log"
rm -f "$LOG"
FAIL=0

send() {  # send <fps> <秒>
    ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=1920x1080:rate=$1" \
        -c:v libx264 -preset veryfast -tune zerolatency -b:v 8M -g "$(($1 * 2))" -bf 0 -pix_fmt yuv420p \
        -t "$2" -f mpegts "srt://127.0.0.1:$PORT?mode=caller&latency=120000&pkt_size=1316" >/dev/null 2>&1
}

check() {  # check <fps>
    local crc="$OUT/vcam_pace_$1.txt"
    send "$1" 16 &
    local sender=$!
    sleep 4
    # サンプルを 1 つずつそのまま数える (-fps_mode passthrough: ffmpeg に複製・間引きをさせない)
    ffmpeg -nostdin -hide_banner -loglevel error -f dshow -rtbufsize 512M -video_size 1920x1080 -framerate 60 -pixel_format nv12 \
        -i video="denpachan Cam 1" -t 10 -an -fps_mode passthrough -f framecrc - >"$crc" 2>/dev/null
    wait "$sender"
    # pts の単位は 1/60 秒。最初の 1 秒 (読み手が映像の間隔を測り終えるまで) は数えない
    grep -v '^#' "$crc" | awk -F', *' -v fps="$1" '
        NR == 1 { first = $2 }
        $2 < first + 60 { prev = $6; next }
        { n++; if ($6 == prev) dup++; prev = $6; last = $2 }
        END {
            sec = (last - first - 60) / 60
            if (n == 0 || sec <= 0) { print "no samples"; exit 1 }
            new = n - dup
            printf "%2d fps: samples %d, same as previous %d (%.1f %%), new pictures %.1f per s\n", fps, n, dup, 100 * dup / n, new / sec
            exit !(dup / n < 0.03 && new / sec > fps * 0.9)
        }'
}

"$BIN/denpachan-worker.exe" --ch 1 --cam 1 --port "$PORT" --log-file "$LOG" >/dev/null 2>&1 &
WORKER=$!
sleep 1

for fps in 30 60; do
    if check "$fps"; then echo "OK"; else echo "FAIL"; FAIL=1; fi
    sleep 2
done

kill -9 "$WORKER" 2>/dev/null
taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1
wait "$WORKER" 2>/dev/null
exit $FAIL
