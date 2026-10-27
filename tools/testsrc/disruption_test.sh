#!/usr/bin/env bash
# 乱れの調べ方の試験: 止める場所を変えて乱れを作り、disruption_report.py がそれぞれを言い当てるかを見る (Python を使う)。
#
# ch1: SRT listener、ch2: RTMP push。送り側は試験用の素材を -readrate_catchup 4 で送る (止めた分を 4 倍速で追いつく)。
#   1. SRT の送り側を止める          → データが届かなかった
#   2. worker ch1 を止める           → プロセスが止まった (worker ch1)
#   3. RTMP の送り側を止める         → データが届かなかった
#   4. rtmpd を止める                → プロセスが止まった (rtmpd)
#   5. denpachan の全プロセスを止める → PC 全体が止まった
# 音声は VB-Audio Virtual Cable (CABLE Input) と VoiceMeeter Input に出す。試験用の設定ファイル (out/check/disruption-config.json) を使う。
#
#   tools/testsrc/disruption_test.sh
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tools/testsrc/dev_env.sh"
export SPDLOG_LEVEL="info,ch1=debug,ch2=debug"
BIN="$ROOT/out/build/bin/Release"
OUT="$ROOT/out/check"
CTL="$BIN/denpachan-ctl.exe"
PWSH="/c/Program Files/PowerShell/7/pwsh.exe"
SUSPEND="$(cygpath -w "$ROOT/tools/testsrc/suspend.ps1")"
MANY="$(cygpath -w "$ROOT/tools/testsrc/suspend_many.ps1")"
LOGS="$(cygpath -u "$LOCALAPPDATA")/denpachan/logs"
PAUSE_MS=800
mkdir -p "$OUT"
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1
taskkill //IM denpachan-rtmpd.exe //F >/dev/null 2>&1
sleep 1
rm -f "$OUT/disruption-config.json"
# engine のログは普段のフォルダ (worker・rtmpd と同じ所) に書く (disruption_report.py がまとめて読む)
"$BIN/denpachan-engine.exe" --config "$OUT/disruption-config.json" >/dev/null 2>&1 &
sleep 2
SINCE=$(date "+%Y-%m-%d %H:%M:%S")
"$CTL" set 1 '{"source":{"protocol":"srt","mode":"listener","port":9001},"audio":{"device":"CABLE Input","startChannel":1}}' >/dev/null
"$CTL" set 2 '{"source":{"protocol":"rtmp","mode":"push","app":"live","key":"diagkey"},"audio":{"device":"VoiceMeeter Input","startChannel":1}}' >/dev/null
"$CTL" start 1 >/dev/null
"$CTL" start 2 >/dev/null
sleep 2

# 先にファイルへエンコードしておき、それを送る (生成しながらだと、止めたときにタイムスタンプが飛ぶ)
SRC="$OUT/disruption_src.ts"
if [ ! -f "$SRC" ]; then
    ffmpeg -hide_banner -loglevel error -f lavfi -i "testsrc2=size=1280x720:rate=60" -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
        -c:v libx264 -preset veryfast -tune zerolatency -b:v 4M -g 120 -bf 0 -pix_fmt yuv420p -c:a aac -b:a 192k -t 30 "$SRC"
fi
ffmpeg -hide_banner -loglevel error -stream_loop -1 -readrate 1 -readrate_catchup 4 -i "$SRC" -c copy -t 200 \
    -f mpegts "srt://127.0.0.1:9001?mode=caller&latency=120000&pkt_size=1316" >/dev/null 2>&1 &
S1=$!
ffmpeg -hide_banner -loglevel error -stream_loop -1 -readrate 1 -readrate_catchup 4 -i "$SRC" -c copy -t 200 \
    -f flv "rtmp://127.0.0.1:1935/live/diagkey" >/dev/null 2>&1 &
S2=$!
sleep 30

pid_of() { "$CTL" summary | grep "^ch$1 " | grep -oE "pid [0-9]+" | awk '{print $2}'; }
image_pid() { tasklist //FI "IMAGENAME eq $1" //FO CSV //NH | head -1 | cut -d, -f2 | tr -d '"\r'; }
ENGINE=$(image_pid denpachan-engine.exe)
RTMPD=$(image_pid denpachan-rtmpd.exe)
W1=$(pid_of 1)
W2=$(pid_of 2)
suspend() { powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$SUSPEND" -ProcessId "$1" -Milliseconds "$PAUSE_MS"; }
step() { echo "$(date +%H:%M:%S) $*"; }
step "1. SRT の送り側を止める"
suspend "$(cat /proc/$S1/winpid)"
sleep 20
step "2. worker ch1 を止める"
suspend "$W1"
sleep 20
step "3. RTMP の送り側を止める"
suspend "$(cat /proc/$S2/winpid)"
sleep 20
step "4. rtmpd を止める"
suspend "$RTMPD"
sleep 20
step "5. denpachan の全プロセスを同時に止める"
"$PWSH" -NoProfile -ExecutionPolicy Bypass -File "$MANY" -ProcessIds "$ENGINE,$RTMPD,$W1,$W2" -Milliseconds "$PAUSE_MS"
sleep 20
UNTIL=$(date "+%Y-%m-%d %H:%M:%S")
"$CTL" stop 1 >/dev/null
"$CTL" stop 2 >/dev/null
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
kill "$S1" "$S2" 2>/dev/null
sleep 1

# Microsoft Store 版の Python は AppData の下が見えないので、ログを写してから渡す
rm -rf "$OUT/disruption-logs"
cp -r "$LOGS" "$OUT/disruption-logs"
python -E "$ROOT/tools/testsrc/disruption_report.py" "$OUT/disruption-logs" --since "$SINCE" --until "$UNTIL" --no-windows-events >"$OUT/disruption_report.txt"
grep -E "^■" "$OUT/disruption_report.txt"

FAIL=0
check() {  # check <説明> <件の行に含まれる文字列>
    if grep -E "^■" "$OUT/disruption_report.txt" | grep -q "$2"; then echo "OK   $1"; else echo "FAIL $1"; FAIL=1; fi
}
[ "$(grep -E "^■" "$OUT/disruption_report.txt" | grep -c "データが届かなかった")" -ge 2 ]
if [ $? = 0 ]; then echo "OK   1・3. 送り側を止めた 2 件は「データが届かなかった」"; else echo "FAIL 1・3. 送り側を止めた 2 件は「データが届かなかった」"; FAIL=1; fi
check "2. worker ch1 を止めたら「プロセスが止まった (worker ch1)」" "プロセスが止まった (denpachan: worker ch1)"
check "4. rtmpd を止めたら「プロセスが止まった (rtmpd)」" "プロセスが止まった (denpachan: rtmpd)"
check "5. 全プロセスを止めたら「PC 全体が止まった」" "PC 全体が止まった"
echo "(詳しくは $OUT/disruption_report.txt)"
exit $FAIL
