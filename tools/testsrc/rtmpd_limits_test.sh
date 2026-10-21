#!/usr/bin/env bash
# rtmpd の守りの試験 (Python を使う)。
#
# 1. publish せずにデータを送り続ける接続は、1 MB を超えたら切られる
# 2. publish せずに小さなメッセージを送り続ける接続は、30 秒で切られる
# 3. publish の途中で rtmpd が落ちても、worker が「接続中」のまま残らない (起動し直した rtmpd が残ったセッションを終わらせる)
# 4. publish の途中で rtmpd が止まって (ハートビートが切れて) 再開したら、worker も同じ publish を読み直す
#    (3 より先に行う)
# 試験用の設定ファイル (out/check/rtmpd-limits-config.json) を使う。
#
#   tools/testsrc/rtmpd_limits_test.sh
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tools/testsrc/dev_env.sh"
BIN="$ROOT/out/build/bin/Release"
OUT="$ROOT/out/check"
CFG="$OUT/rtmpd-limits-config.json"
CTL="$BIN/denpachan-ctl.exe"
LOGS="$(cygpath -u "$LOCALAPPDATA")/denpachan/logs"
mkdir -p "$OUT"
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1
taskkill //IM denpachan-rtmpd.exe //F >/dev/null 2>&1
sleep 1
rm -f "$CFG"
SKIP_R=$(cat "$LOGS/rtmpd.log" 2>/dev/null | wc -l)
SKIP_C=$(cat "$LOGS/ch1.log" 2>/dev/null | wc -l)
FAIL=0
check() {  # check <説明> <0 = OK>
    if [ "$2" = 0 ]; then echo "OK   $1"; else echo "FAIL $1"; FAIL=1; fi
}

"$BIN/denpachan-engine.exe" --config "$CFG" --log-file "$OUT/rtmpd-limits-engine.log" >/dev/null 2>&1 &
sleep 2
"$CTL" set 1 '{"source":{"protocol":"rtmp","mode":"push","app":"live","key":"limkey"},"vcam":0}' >/dev/null
"$CTL" start 1
sleep 3

R1=$(python -I "$ROOT/tools/testsrc/rtmp_misbehave.py" 127.0.0.1 1935 live flood)
echo "  $R1"
grep -qE "closed by the server after ([0-9]+) bytes" <<<"$R1" && [ "$(grep -oE "after [0-9]+" <<<"$R1" | grep -oE "[0-9]+")" -lt 2000000 ]
check "1. publish せずに送り続けると 1 MB ほどで切られる" $?

R2=$(python -I "$ROOT/tools/testsrc/rtmp_misbehave.py" 127.0.0.1 1935 live idle)
echo "  $R2"
SEC=$(grep -oE "[0-9.]+ s$" <<<"$R2" | grep -oE "^[0-9]+")
grep -q "closed by the server" <<<"$R2" && [ "${SEC:-99}" -ge 28 ] && [ "${SEC:-99}" -le 36 ]
check "2. publish しないまま 30 秒経つと切られる (got ${SEC:-?} s)" $?

# 4. publish の途中で rtmpd を 4 秒止める (ハートビートが 3 秒を超えて止まる)。再開したら worker も読み直す
ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=1280x720:rate=30" -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
    -c:v libx264 -preset veryfast -tune zerolatency -b:v 2M -g 30 -bf 0 -pix_fmt yuv420p -c:a aac -b:a 128k -t 20 \
    -f flv "rtmp://127.0.0.1:1935/live/limkey" >/dev/null 2>&1 &
SENDER=$!
sleep 5
RTMPD_PID=$(tasklist //FI "IMAGENAME eq denpachan-rtmpd.exe" //FO CSV //NH | head -1 | cut -d, -f2 | tr -d '"\r')
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w "$ROOT/tools/testsrc/suspend.ps1")" -ProcessId "$RTMPD_PID" -Milliseconds 4000
sleep 6
LINE=$("$CTL" summary | head -1)
echo "  $LINE"
STATE=$(grep -oE "video [a-z]+ [a-z]+" <<<"$LINE" | awk '{print $2}')
[ "$STATE" = live ]
check "4. rtmpd が 4 秒止まって再開したら、映像はまた受信中になる (got ${STATE:-?})" $?
wait "$SENDER" 2>/dev/null
sleep 3

# 3. publish の途中で rtmpd を落とす (engine が起動し直す)。送出側は落ちたら終わる
ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=1280x720:rate=30" -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
    -c:v libx264 -preset veryfast -tune zerolatency -b:v 2M -g 30 -bf 0 -pix_fmt yuv420p -c:a aac -b:a 128k -t 30 \
    -f flv "rtmp://127.0.0.1:1935/live/limkey" >/dev/null 2>&1 &
SENDER=$!
sleep 6
"$CTL" summary | head -1
taskkill //IM denpachan-rtmpd.exe //F >/dev/null 2>&1
wait "$SENDER" 2>/dev/null
sleep 5
LINE=$("$CTL" summary | head -1)
echo "  $LINE"
# 要約の "video <映像の状態> <接続の状態>" の接続の状態を見る
CONN=$(grep -oE "video [a-z]+ [a-z]+" <<<"$LINE" | awk '{print $3}')
[ -n "$CONN" ] && [ "$CONN" != connected ]
check "3. rtmpd が落ちて起動し直したあと、worker は「接続中」のまま残らない (got ${CONN:-?})" $?

"$CTL" stop 1 >/dev/null
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
echo "---- rtmpd log"
tail -n +"$((SKIP_R + 1))" "$LOGS/rtmpd.log" | grep -E "closing|publish" | cut -c12-
echo "---- ch1 log"
tail -n +"$((SKIP_C + 1))" "$LOGS/ch1.log" | grep -E "RTMP:" | cut -c12-
exit $FAIL
