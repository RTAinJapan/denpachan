#!/usr/bin/env bash
# 録画の試験 (パススルー / 再エンコード、映像の途切れでの区切り、ファイル名テンプレート)。
#
# 試験用の設定ファイル (out/check/record-config.json) を使うので、普段の設定は触らない。
# 録画は out/check/rec に書く。
#
#   tools/testsrc/record_test.sh [encoder]    encoder: nvenc (既定) / qsv / auto
set -u

ENCODER="${1:-nvenc}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tools/testsrc/dev_env.sh"
BIN="$ROOT/out/build/bin/Release"
OUT="$ROOT/out/check"
CFG="$OUT/record-config.json"
LOG="$OUT/record-engine.log"
REC="$OUT/rec"
CTL="$BIN/denpachan-ctl.exe"
mkdir -p "$OUT"
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1
sleep 1
rm -rf "$CFG" "$LOG" "$REC"

step() { echo; echo "==== $*"; }
summary() { "$CTL" summary | head -1; }
worker_pid() { "$CTL" summary | head -1 | grep -oE "pid [0-9]+" | cut -d' ' -f2; }
rec_files() { find "$REC" -type f \( -name "*.mkv" -o -name "*.ts" \) | sort; }
probe() {
    local f="$1"
    echo "  ${f#$REC/}"
    echo "    $(ffprobe -v error -show_entries format=duration,size -of compact=p=0 "$f")"
    ffprobe -v error -show_entries stream=codec_type,codec_name,width,height,sample_rate,channels -of compact=p=0 "$f" | sed 's/^/    /'
    # 先頭から最後まで読めるか (壊れたパケットがないか)
    local errors
    errors=$(ffmpeg -hide_banner -v error -i "$f" -f null - 2>&1 | wc -l)
    echo "    decode errors: $errors"
}

send() {
    ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=1280x720:rate=60" \
        -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
        -c:v libx264 -preset veryfast -tune zerolatency -b:v 4M -g 60 -bf 0 -pix_fmt yuv420p \
        -c:a aac -b:a 192k -pes_payload_size 0 -muxdelay 0 -muxpreload 0 -t "$1" \
        -f mpegts "srt://127.0.0.1:9001?mode=caller&latency=120000&pkt_size=1316" >/dev/null 2>&1
}

START=$(date +%H:%M)
"$BIN/denpachan-engine.exe" --config "$CFG" --log-file "$LOG" >/dev/null 2>&1 &
sleep 2

step "1. 録画の設定 (保存先、テンプレートに / と :、途切れ 3 秒で区切る)"
"$CTL" set 1 "{\"record\":{\"dir\":\"$(cygpath -w "$REC" | sed 's/\\/\\\\/g')\",\"template\":\"{ch}/{name} {YYYY}-{MM}-{DD} {hh}:{mm}:{ss} {codec} {res}\",\"invalidCharReplacement\":\"-\",\"gapTimeoutSec\":3}}" |
    grep -E '"(dir|template|gapTimeoutSec)"'
"$CTL" start 1
sleep 2
PID=$(worker_pid)

step "2. 録画オン (worker は再起動しない) → 8 秒受信"
"$CTL" record 1 on
send 8 &
SENDER=$!
sleep 5
summary
[ "$(worker_pid)" = "$PID" ] && echo "OK: worker kept running (pid $PID)" || echo "FAIL: worker restarted"
wait "$SENDER"

step "3. 2 秒切れて (区切り時間より短い) 再接続 → 同じファイルに続けて書く"
sleep 2
send 6
summary
sleep 5
echo "files after the gap:"
rec_files
[ "$(rec_files | wc -l)" = "1" ] && echo "OK: one file for both connections" || echo "FAIL: file was split"

step "4. 3 秒以上途切れたあとの受信 → 新しいファイル"
send 5
sleep 5
rec_files
[ "$(rec_files | wc -l)" = "2" ] && echo "OK: split into a new file" || echo "FAIL: expected 2 files"

step "5. 再エンコード ($ENCODER, HEVC, CBR 6 Mbps) に切り替え → 6 秒受信"
"$CTL" set 1 "{\"record\":{\"mode\":\"encode\",\"encode\":{\"video\":{\"codec\":\"hevc\",\"encoder\":\"$ENCODER\",\"rc\":\"cbr\",\"bitrateKbps\":6000}}}}" >/dev/null
send 6 &
SENDER=$!
sleep 4
summary
wait "$SENDER"
sleep 5
[ "$(worker_pid)" = "$PID" ] && echo "OK: worker kept running (pid $PID)" || echo "FAIL: worker restarted"

step "5b. 使えないエンコーダ (AMF) → エラー表示、受信は続く、ログは 10 秒に 1 回"
"$CTL" set 1 '{"record":{"encode":{"video":{"encoder":"amf"}}}}' >/dev/null
send 4 &
SENDER=$!
sleep 3
summary
wait "$SENDER"
"$CTL" set 1 '{"record":{"encode":{"video":{"encoder":"auto"}}}}' >/dev/null
sleep 4

step "5c. CPU エンコード (外部 ffmpeg の x264, CBR 6 Mbps, p3) → 6 秒受信"
"$CTL" set 1 '{"record":{"encode":{"video":{"codec":"h264","encoder":"cpu","preset":"p3"}}}}' >/dev/null
send 6 &
SENDER=$!
sleep 4
summary
tasklist //FI "IMAGENAME eq ffmpeg.exe" //FO CSV //NH | grep -c ffmpeg | sed 's/^/ffmpeg processes (sender + encoder): /'
wait "$SENDER"
sleep 5
[ "$(worker_pid)" = "$PID" ] && echo "OK: worker kept running (pid $PID)" || echo "FAIL: worker restarted"

step "5d. CPU エンコード (x265, 固定 QP 28) → 5 秒受信"
"$CTL" set 1 '{"record":{"encode":{"video":{"codec":"hevc","rc":"cqp","qp":28}}}}' >/dev/null
send 5
sleep 5

step "5e. ffmpeg が見つからない → エラー表示、受信は続く"
"$CTL" set-general '{"ffmpegPath":"C:\\nowhere\\ffmpeg.exe"}' >/dev/null
send 4 &
SENDER=$!
sleep 3
summary
wait "$SENDER"
"$CTL" set-general '{"ffmpegPath":""}' >/dev/null
sleep 4

step "6. MPEG-TS / パススルー に切り替え → 4 秒受信"
"$CTL" set 1 '{"record":{"mode":"passthrough","container":"ts"}}' >/dev/null
send 4
sleep 5

step "7. 録画オフ"
"$CTL" record 1 off
sleep 1
summary

step "8. 録画されたファイル"
rec_files | while read -r f; do probe "$f"; done

"$CTL" stop 1
taskkill //IM ffmpeg.exe //F >/dev/null 2>&1
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
echo
echo "==== worker log (recorder, this run)"
grep -iE "record:" "$(cygpath -u "$LOCALAPPDATA")/denpachan/logs/ch1.log" 2>/dev/null | cut -c12- | awk -v s="$START" 'substr($0, 1, 5) >= s' | tail -30
