#!/usr/bin/env bash
# 同じ SRT のポート / 同じ RTMP のキーに、2 つの送出元がつないだときの試験。
#
# 送出元 A を送り続け、2 秒後から送出元 B を「切れたら繋ぎ直す」で送り続ける。
# - A が送っている間は B が断られ、接続は A のまま (交互に切り替わり続けない)。断ったログは 10 秒に 1 行にまとまる
# - A を 4 秒止める (半開きの代わり) と、B に置き換わる
# を、SRT (worker 単体) と RTMP (engine + rtmpd) で確かめる。試験用の設定ファイル (out/check/dup-config.json) を使う。
#
#   tools/testsrc/duplicate_sender_test.sh
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tools/testsrc/dev_env.sh"
BIN="$ROOT/out/build/bin/Release"
OUT="$ROOT/out/check"
CTL="$BIN/denpachan-ctl.exe"
CFG="$OUT/dup-config.json"
WLOG="$OUT/dup_worker.log"
RTMPD_LOG="$(cygpath -u "$LOCALAPPDATA")/denpachan/logs/rtmpd.log"
mkdir -p "$OUT"
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1
taskkill //IM denpachan-rtmpd.exe //F >/dev/null 2>&1
sleep 1
rm -f "$CFG" "$WLOG"
FAIL=0
# 断った回数の合計 (ログは 10 秒に 1 行にまとめて「rejected N ...」と回数を添える)
rejected_total() { grep -oE "rejected [0-9]+" | awk '{ n += $2 } END { print n + 0 }'; }
check() {  # check <説明> <0 = OK>
    if [ "$2" = 0 ]; then echo "OK   $1"; else echo "FAIL $1"; FAIL=1; fi
}

SRC=(-re -f lavfi -i "testsrc2=size=1280x720:rate=30" -f lavfi -i "sine=frequency=1000:sample_rate=48000"
    -c:v libx264 -preset veryfast -tune zerolatency -b:v 2M -g 30 -bf 0 -pix_fmt yuv420p -c:a aac -b:a 128k)
SRT_OUT=(-f mpegts "srt://127.0.0.1:9001?mode=caller&latency=120000&pkt_size=1316")
RTMP_OUT=(-f flv "rtmp://127.0.0.1:1935/live/dupkey")
pause() {  # pause <pid (bash)> <ms>
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$ROOT/tools/testsrc/suspend.ps1" -ProcessId "$(cat /proc/$1/winpid)" -Milliseconds "$2"
}

# 試験の流れ (出力先を引数で受ける)。A を 20 秒、B を繋ぎ直しながら。8 秒後に A を 4 秒止める
run() {  # run <ffmpeg の出力の引数...>
    # A は ffmpeg を直接裏で動かす (関数を裏で動かすと $! が bash になり、止めても ffmpeg は止まらない)
    ffmpeg -hide_banner -loglevel error "${SRC[@]}" -t 20 "$@" >/dev/null 2>&1 &
    local a=$!
    sleep 2
    ( end=$((SECONDS + 16)); while [ $SECONDS -lt $end ]; do ffmpeg -hide_banner -loglevel error "${SRC[@]}" -t 3 "$@" >/dev/null 2>&1; sleep 0.5; done ) &
    local b=$!
    sleep 6
    MARK_PAUSE=$(date +%H:%M:%S)
    pause "$a" 4000
    sleep 4
    kill "$b" 2>/dev/null
    taskkill //IM ffmpeg.exe //F >/dev/null 2>&1
    wait "$a" "$b" 2>/dev/null  # 裏で動かしている worker / engine は待たない
}

echo "==== SRT (listener)"
"$BIN/denpachan-worker.exe" --ch 1 --cam 0 --port 9001 --log-file "$WLOG" >/dev/null 2>&1 &
sleep 1
run "${SRT_OUT[@]}"
taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1
BEFORE=$(awk -v t="$MARK_PAUSE" 'substr($2, 1, 8) < t' "$WLOG")
echo "  A が送っている間: 接続 $(grep -c "SRT: connected from" <<<"$BEFORE")、置き換え $(grep -c "replaces" <<<"$BEFORE")"
echo "  断った (全体): $(rejected_total <"$WLOG") 回、ログ $(grep -c "rejected" "$WLOG") 行"
[ "$(grep -c "SRT: connected from" <<<"$BEFORE")" = 1 ] && [ "$(grep -c "rejected" <<<"$BEFORE")" -ge 1 ]
check "SRT: A が送っている間は B を断り、A のまま" $?
grep -q "replaces the current one (no data" "$WLOG"
check "SRT: A が止まると B に置き換わる" $?

echo "==== RTMP (push)"
SKIP=$(cat "$RTMPD_LOG" 2>/dev/null | wc -l)
"$BIN/denpachan-engine.exe" --config "$CFG" --log-file "$OUT/dup-engine.log" >/dev/null 2>&1 &
sleep 2
"$CTL" set 1 '{"source":{"protocol":"rtmp","mode":"push","app":"live","key":"dupkey"},"vcam":0}' >/dev/null
"$CTL" start 1
sleep 3
run "${RTMP_OUT[@]}"
"$CTL" stop 1 >/dev/null
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
LOG=$(tail -n +"$((SKIP + 1))" "$RTMPD_LOG")
BEFORE=$(awk -v t="$MARK_PAUSE" 'substr($2, 1, 8) < t' <<<"$LOG")
echo "  A が送っている間: publish $(grep -c "RTMP publish from" <<<"$BEFORE")、置き換え $(grep -c "replaces" <<<"$BEFORE")"
echo "  断った (全体): $(rejected_total <<<"$LOG") 回、ログ $(grep -c "rejected" <<<"$LOG") 行"
[ "$(grep -c "RTMP publish from" <<<"$BEFORE")" = 1 ] && [ "$(grep -c "rejected" <<<"$BEFORE")" -ge 1 ]
check "RTMP: A が送っている間は B を断り、A のまま" $?
grep -q "replaces .* (no data" <<<"$LOG"
check "RTMP: A が止まると B に置き換わる" $?
exit $FAIL
