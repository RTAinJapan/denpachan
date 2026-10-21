#!/usr/bin/env bash
# 録画中に、同じ接続のままタイムスタンプが戻ったときの試験 (送出側の再起動、MPEG-TS の折り返しなどの代わり)。
#
# worker の試験用オプション --test-ts-rewind で、受信してから 6 秒後にタイムスタンプを 8 秒戻す。
# パススルーと再エンコード (GPU) で 14 秒ずつ録画し、戻った後も映像が正しい間隔で続き、音声も途切れないことを確かめる。
# 試験用の設定ファイル (out/check/rewind-config.json) を使うので、普段の設定は触らない。録画は out/check/rewind-rec に書く。
#
#   tools/testsrc/rewind_test.sh [encoder]    encoder: nvenc (既定) / qsv / auto
set -u

ENCODER="${1:-nvenc}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tools/testsrc/dev_env.sh"
BIN="$ROOT/out/build/bin/Release"
OUT="$ROOT/out/check"
CFG="$OUT/rewind-config.json"
LOG="$OUT/rewind-engine.log"
REC="$OUT/rewind-rec"
MARK="$OUT/rewind-marker"
CTL="$BIN/denpachan-ctl.exe"
mkdir -p "$OUT"
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1
sleep 1
rm -rf "$CFG" "$LOG" "$REC"
FAIL=0

send() {
    ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=1280x720:rate=60" \
        -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
        -c:v libx264 -preset veryfast -tune zerolatency -b:v 4M -g 60 -bf 0 -pix_fmt yuv420p \
        -c:a aac -b:a 192k -pes_payload_size 0 -muxdelay 0 -muxpreload 0 -t "$1" \
        -f mpegts "srt://127.0.0.1:9001?mode=caller&latency=120000&pkt_size=1316" >/dev/null 2>&1
}

# 録画ファイルの映像・音声のパケットの間隔を調べる。
# 映像: 1 フレーム (16.7 ms) の半分より詰まった間隔がない、0.5 秒を超える間がない。音声: 0.5 秒を超える間がない、最後まで続く
check_file() {  # check_file <説明> <ファイル>
    local f=$2
    echo "  ${f#$REC/}: $(ffprobe -v error -show_entries format=duration -of csv=p=0 "$f") s"
    ffprobe -v error -show_entries packet=stream_index,pts_time -of csv=p=0 "$f" |
        sort -t, -k1,1n -k2,2g | awk -F, -v name="$1" '
        $2 == "N/A" { next }
        {
            s = $1; t = $2 + 0
            n[s]++
            if (s in last) {
                d = t - last[s]
                if (d > maxgap[s]) maxgap[s] = d
                if (d < 0.008) packed[s]++
            }
            if (!(s in first)) first[s] = t
            last[s] = t
        }
        END {
            ok = 1
            for (s = 0; s <= 1; s++) {
                kind = s == 0 ? "video" : "audio"
                printf "    %s: %d packets, %.2f-%.2f s, largest gap %.3f s, packed %d\n", kind, n[s], first[s], last[s], maxgap[s], packed[s] + 0
                if (n[s] == 0 || maxgap[s] > 0.5) ok = 0
                if (s == 0 && packed[s] > 0) ok = 0
            }
            if (last[1] < last[0] - 1) ok = 0  # 音声が映像より 1 秒以上早く終わっている
            print (ok ? "OK   " : "FAIL ") name
            exit !ok
        }'
}

"$BIN/denpachan-engine.exe" --config "$CFG" --log-file "$LOG" >/dev/null 2>&1 &
sleep 2
"$CTL" set 1 "{\"debugArgs\":\"--test-ts-rewind 6\",\"record\":{\"enabled\":true,\"dir\":\"$(cygpath -w "$REC" | sed 's/\\/\\\\/g')\",\"template\":\"{ch} {hh}{mm}{ss}\",\"gapTimeoutSec\":5}}" >/dev/null

run_case() {  # run_case <説明> <record の JSON>
    echo "==== $1"
    touch "$MARK"  # このケースのファイルを見分ける
    sleep 1
    "$CTL" set 1 "{\"record\":$2}" >/dev/null
    "$CTL" start 1
    sleep 2
    send 14
    sleep 7  # 途切れ (5 秒) でファイルが閉じるまで
    "$CTL" stop 1
    sleep 1
    local f
    f=$(find "$REC" -type f -newer "$MARK" -name "*.mkv" | sort | tail -1)
    if [ -z "$f" ]; then echo "FAIL $1: no file"; FAIL=1; return; fi
    check_file "$1" "$f" || FAIL=1
}

run_case "パススルー" '{"mode":"passthrough","container":"mkv"}'
run_case "再エンコード ($ENCODER)" "{\"mode\":\"encode\",\"container\":\"mkv\",\"encode\":{\"video\":{\"codec\":\"h264\",\"encoder\":\"$ENCODER\"}}}"

taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
echo
echo "==== worker log (this run)"
grep -E "test: timestamps|record:|re-anchor" "$(cygpath -u "$LOCALAPPDATA")/denpachan/logs/ch1.log" | tail -12 | cut -c12-
exit $FAIL
