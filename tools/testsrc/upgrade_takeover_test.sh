#!/usr/bin/env bash
# 前の版から engine だけを入れ替えたときの試験 (worker と rtmpd は動いたまま残る)。
#
# 前の版 (別のフォルダに展開した配布版) の engine で、ch1 (SRT listener 9001) と ch2 (RTMP push) を受信する。
# 前の版の engine だけを強制終了し、この版の engine を同じ設定で起動する。
# - 前の版の worker / rtmpd が止まり、この版のものが 1 つずつ動いている (二重に動かない)
# - RTMP の振り分けの共有メモリを開ける (ログに "cannot open RTMP routes" が出ない)
# - ch1 (SRT) と ch2 (RTMP) を受信できる
# を確かめる。試験用の設定ファイル (out/check/upgrade2-config.json) を使う。
#
#   tools/testsrc/upgrade_takeover_test.sh [前の版のフォルダ (既定 out/release-check/app/denpachan-0.1.0)]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tools/testsrc/dev_env.sh"
OLD="${1:-$ROOT/out/release-check/app/denpachan-0.1.0}"
BIN="$ROOT/out/build/bin/Release"
OUT="$ROOT/out/check"
CFG="$OUT/upgrade2-config.json"
NEW_LOG="$OUT/upgrade2-engine.log"
mkdir -p "$OUT"
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1
taskkill //IM denpachan-rtmpd.exe //F >/dev/null 2>&1
sleep 1
rm -f "$CFG" "$NEW_LOG" "$OUT/upgrade2-old-engine.log"
FAIL=0
check() {  # check <説明> <0 = OK>
    if [ "$2" = 0 ]; then echo "OK   $1"; else echo "FAIL $1"; FAIL=1; fi
}
# 動いているプロセスの実行ファイルの場所 (名前ごと)
procs() {
    powershell.exe -NoProfile -Command "Get-Process denpachan-worker, denpachan-rtmpd -ErrorAction SilentlyContinue | ForEach-Object { \$_.Path }" | tr -d '\r'
}
SRC=(-re -f lavfi -i "testsrc2=size=1280x720:rate=30" -f lavfi -i "sine=frequency=1000:sample_rate=48000"
    -c:v libx264 -preset veryfast -tune zerolatency -b:v 2M -g 30 -bf 0 -pix_fmt yuv420p -c:a aac -b:a 128k)

echo "==== 1. 前の版 ($OLD) で ch1 (SRT) と ch2 (RTMP) を受信"
"$OLD/denpachan-engine.exe" --config "$CFG" --log-file "$OUT/upgrade2-old-engine.log" >/dev/null 2>&1 &
sleep 2
"$OLD/denpachan-ctl.exe" set 1 '{"source":{"protocol":"srt","mode":"listener","port":9001},"vcam":0}' >/dev/null
"$OLD/denpachan-ctl.exe" set 2 '{"source":{"protocol":"rtmp","mode":"push","app":"live","key":"upkey"},"vcam":0}' >/dev/null
"$OLD/denpachan-ctl.exe" start 1
"$OLD/denpachan-ctl.exe" start 2
sleep 3
procs

echo "==== 2. 前の版の engine だけ強制終了 → この版の engine を起動"
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
sleep 1
"$BIN/denpachan-engine.exe" --config "$CFG" --log-file "$NEW_LOG" >/dev/null 2>&1 &
sleep 4
P=$(procs)
echo "$P"
[ "$(grep -ci "denpachan-worker.exe" <<<"$P")" = 2 ] && [ "$(grep -ci "denpachan-rtmpd.exe" <<<"$P")" = 1 ] && ! grep -qiF "$(cygpath -w "$OLD")" <<<"$P"
check "前の版の worker / rtmpd は止まり、この版のものが二重にならずに動いている" $?
! grep -q "cannot open RTMP routes" "$NEW_LOG"
check "RTMP の振り分けの共有メモリを開けた" $?

echo "==== 3. 受信できるか"
ffmpeg -hide_banner -loglevel error "${SRC[@]}" -t 8 -f mpegts "srt://127.0.0.1:9001?mode=caller&latency=120000&pkt_size=1316" >/dev/null 2>&1 &
S1=$!
ffmpeg -hide_banner -loglevel error "${SRC[@]}" -t 8 -f flv "rtmp://127.0.0.1:1935/live/upkey" >/dev/null 2>&1 &
S2=$!
sleep 6
SUM=$("$BIN/denpachan-ctl.exe" summary)
echo "$SUM" | head -2
[ "$(echo "$SUM" | head -2 | grep -c "video live")" = 2 ]
check "ch1 (SRT) と ch2 (RTMP) を受信できる" $?
wait "$S1" "$S2"

"$BIN/denpachan-ctl.exe" stop 1 >/dev/null
"$BIN/denpachan-ctl.exe" stop 2 >/dev/null
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
echo "---- この版の engine のログ"
grep -E "leftover|routes|adopted|started|warn|error" "$NEW_LOG" | cut -c12- | head -20
exit $FAIL
