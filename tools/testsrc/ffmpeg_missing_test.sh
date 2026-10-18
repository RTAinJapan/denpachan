#!/usr/bin/env bash
# FFmpeg (同梱しない) が無いときの試験。
#
# 1. FFmpeg の DLL がどこにも無い状態で engine を起動 → engine と UI 用の状態は動き、チャンネルは「FFmpeg なし」
#    (worker を起動しない。再起動を繰り返さない)
# 2. 動いている間に FFmpeg の DLL を denpachan と同じフォルダに置く → 10 秒以内に見つけて受信を始める
# 3. 片付け (置いた DLL を消す)
#
#   tools/testsrc/ffmpeg_missing_test.sh
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/out/build/bin/Release"
OUT="$ROOT/out/check"
CFG="$OUT/ffmpeg-missing-config.json"
LOG="$OUT/ffmpeg-missing-engine.log"
CTL="$BIN/denpachan-ctl.exe"
FFMPEG_DEV_BIN="$ROOT/out/build/vcpkg_installed/x64-windows/bin"
DLLS="avcodec-63.dll avformat-63.dll avutil-61.dll swresample-7.dll swscale-10.dll libvpl.dll"
mkdir -p "$OUT"
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1
sleep 1
rm -f "$CFG" "$LOG"
for dll in $DLLS; do rm -f "$BIN/$dll"; done

step() { echo; echo "==== $*"; }
summary() { "$CTL" summary | head -1; }

step "0. FFmpeg の DLL が PATH にも denpachan のフォルダにも無いこと"
found=0
IFS=: read -ra dirs <<< "$PATH"
for d in "${dirs[@]}" "$BIN"; do
    [ -f "$d/avcodec-63.dll" ] && { echo "found: $d/avcodec-63.dll"; found=1; }
done
[ "$found" = 0 ] && echo "OK: avcodec-63.dll not found" || { echo "FAIL: remove it from PATH first"; exit 1; }

"$BIN/denpachan-engine.exe" --config "$CFG" --log-file "$LOG" >/dev/null 2>&1 &
sleep 2

step "1. FFmpeg なしで ch1 を開始 → 「unavailable」、worker は起動しない"
"$CTL" start 1
sleep 3
summary
"$CTL" state | grep -A2 '"ffmpeg"' | head -3
sleep 3
WORKERS=$(tasklist //FI "IMAGENAME eq denpachan-worker.exe" //NH | grep -c denpachan-worker)
[ "$WORKERS" = 0 ] && echo "OK: no worker was started" || echo "FAIL: $WORKERS worker(s) running"
"$BIN/denpachan.exe" --screenshot "$OUT/ui-ffmpeg-missing.png" --delay 3
echo "screenshot: out/check/ui-ffmpeg-missing.png"

step "2. 動いている間に FFmpeg の DLL を置く → 10 秒以内に見つけて受信を始める"
for dll in $DLLS; do cp "$FFMPEG_DEV_BIN/$dll" "$BIN/"; done
# 待ち受けが始まるまで繋がらないので、繋がるまで送り直す
send_loop() {
    while true; do
        ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=1280x720:rate=60" \
            -c:v libx264 -preset veryfast -tune zerolatency -b:v 4M -g 60 -bf 0 -pix_fmt yuv420p -t 30 \
            -f mpegts "srt://127.0.0.1:9001?mode=caller&latency=120000&pkt_size=1316" >/dev/null 2>&1
        sleep 0.5
    done
}
send_loop &
SENDER=$!
sleep 15
summary
summary | grep -q "video live" && echo "OK: receiving after FFmpeg was found" || echo "FAIL: not receiving"

step "3. 片付け"
"$CTL" stop 1
kill "$SENDER" 2>/dev/null
taskkill //IM ffmpeg.exe //F >/dev/null 2>&1
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
sleep 1
for dll in $DLLS; do rm -f "$BIN/$dll"; done
ls "$BIN"/*.dll | sed "s|$BIN/|  |"

echo
echo "==== engine log"
grep -E "FFmpeg|worker" "$LOG" | cut -c12- | tail -10
