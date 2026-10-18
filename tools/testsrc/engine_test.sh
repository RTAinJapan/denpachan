#!/usr/bin/env bash
# engine の監視・引き継ぎ・設定変更の試験。
#
# 試験用の設定ファイル (out/check/engine-config.json) を使うので、普段の設定は触らない。
# 音声は "CABLE Input" (VB-Audio Virtual Cable) に出す。
#
#   tools/testsrc/engine_test.sh
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tools/testsrc/dev_env.sh"
BIN="$ROOT/out/build/bin/Release"
OUT="$ROOT/out/check"
CFG="$OUT/engine-config.json"
LOG="$OUT/engine.log"
CTL="$BIN/denpachan-ctl.exe"
mkdir -p "$OUT"
rm -f "$CFG" "$LOG"
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1

start_engine() {
    "$BIN/denpachan-engine.exe" --config "$CFG" --log-file "$LOG" >/dev/null 2>&1 &
    sleep 2
}
step() { echo; echo "==== $*"; }
summary() { "$CTL" summary | head -1; }
worker_pid() { "$CTL" summary | head -1 | grep -oE "pid [0-9]+" | cut -d' ' -f2; }

send() {
    ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=1280x720:rate=60" \
        -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
        -c:v libx264 -preset veryfast -tune zerolatency -b:v 4M -g 120 -bf 0 -pix_fmt yuv420p \
        -c:a aac -b:a 192k -pes_payload_size 0 -muxdelay 0 -muxpreload 0 -t "$1" \
        -f mpegts "srt://127.0.0.1:9001?mode=caller&latency=120000&pkt_size=1316" >/dev/null 2>&1
}
# 送出は切断されたら繋ぎ直し続ける (worker の再起動をまたぐため)
send_loop() { while true; do send 600; sleep 0.5; done; }

start_engine

step "1. ch1 の音声出力先を CABLE Input にして待ち受け開始"
"$CTL" set 1 '{"audio":{"device":"CABLE Input"}}' >/dev/null
"$CTL" start 1
send_loop &
SENDER=$!
sleep 7
summary

step "2. worker を強制終了 → 自動で再起動"
PID=$(worker_pid)
taskkill //PID "$PID" //F >/dev/null
sleep 6
summary

step "3. worker の出力スレッドを止める → 固まったことを検出して再起動"
"$CTL" set 1 '{"debugArgs":"--test-hang-playout 3"}' >/dev/null
sleep 11
summary
"$CTL" set 1 '{"debugArgs":null}' >/dev/null
sleep 6
summary

step "4. engine を強制終了 → worker は動き続ける → engine 再起動で引き継ぐ"
PID=$(worker_pid)
taskkill //IM denpachan-engine.exe //F >/dev/null
sleep 2
if tasklist //FI "PID eq $PID" | grep -q denpachan-worker; then echo "worker $PID still running after engine was killed"; else echo "FAIL: worker stopped"; fi
start_engine
sleep 1
summary
if [ "$(worker_pid)" = "$PID" ]; then echo "OK: adopted the same worker (pid $PID)"; else echo "FAIL: worker was restarted"; fi

step "5. 設定変更 (再生バッファ 16ms) → worker を再起動"
"$CTL" set 1 '{"playoutBufferMs":16}' >/dev/null
sleep 6
summary
if [ "$(worker_pid)" != "$PID" ]; then echo "OK: restarted with new settings"; else echo "FAIL: not restarted"; fi

step "6. ミュート on → off (worker は再起動しない)"
PID=$(worker_pid)
"$CTL" mute 1 on
sleep 2
summary
"$CTL" mute 1 off
sleep 2
summary
if [ "$(worker_pid)" = "$PID" ]; then echo "OK: worker kept running"; else echo "FAIL: worker restarted"; fi

step "7. 設定の検証 (ポート重複は拒否される)"
"$CTL" set 2 '{"source":{"port":9001}}'

step "8. パスフレーズは暗号化して保存され、IPC では返らない"
"$CTL" set 2 '{"source":{"passphrase":"0123456789abcdef"}}' | grep -E "Passphrase"
grep -oE '"passphrase[A-Za-z]*": "[^"]{0,24}' "$CFG" | head -4

step "9. engine と worker を両方強制終了 → engine 起動で待ち受けを再開 (設定の復元)"
taskkill //IM denpachan-engine.exe //F >/dev/null
taskkill //IM denpachan-worker.exe //F >/dev/null
sleep 1
start_engine
sleep 6
summary

step "10. 停止"
"$CTL" stop 1
sleep 1
summary

kill "$SENDER" 2>/dev/null
taskkill //IM ffmpeg.exe //F >/dev/null 2>&1
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
echo
echo "==== engine log (warnings)"
grep -E "warn|error|adopted|started|stopped" "$LOG" | cut -c12- | tail -25
