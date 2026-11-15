#!/usr/bin/env bash
# 同じ接続のままタイムスタンプが戻ったときの試験 (送出側の再起動、MPEG-TS の折り返しなどの代わり)。
#
# 1. 録画中に戻る: worker の試験用オプション --test-ts-rewind で、受信してから 6 秒後にタイムスタンプを 8 秒戻す。
#    パススルーと再エンコード (GPU) で 14 秒ずつ録画し、戻った後も映像が正しい間隔で続き、音声も途切れないことを確かめる。
# 2. 途絶のあとに戻る (配信の始め直し): 配信者が配信を止めて始め直し、中継 (AWS MediaConnect など) が接続を保ったまま
#    新しい配信を流すとき。--test-ts-rewind-gap で、戻す前の 3 秒間は受信を捨て (途絶になる)、--test-ts-rewind-by で
#    600 秒戻す (前の配信が続いていた長さ)。送出は B フレームあり (OBS の既定と同じく、デコーダが並べ替えの分の
#    フレームを持つ)。デコーダに残っていた戻る前の映像を、新しい時間軸のとおりに待つと、戻った幅だけ映像が止まる。
# どの場合も、戻ってから 2 秒後には映像の出力が戻っていること (出力の fps が受信の fps の半分以上) を、worker の
# 1 秒ごとのログで確かめる。
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
CH_LOG="$(cygpath -u "$LOCALAPPDATA")/denpachan/logs/ch1.log"
RUN_LOG="$OUT/rewind-ch1.log"  # この回の worker のログ
mkdir -p "$OUT"
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1
sleep 1
rm -rf "$CFG" "$LOG" "$REC" "$RUN_LOG"
FAIL=0

send() {  # send <秒> [映像のエンコードの引数...] (既定は B フレームなし)
    local t=$1
    shift
    [ $# -eq 0 ] && set -- -tune zerolatency -bf 0
    ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=1280x720:rate=60" \
        -f lavfi -i "sine=frequency=1000:sample_rate=48000" \
        -c:v libx264 -preset veryfast "$@" -b:v 4M -g 60 -pix_fmt yuv420p \
        -c:a aac -b:a 192k -pes_payload_size 0 -muxdelay 0 -muxpreload 0 -t "$t" \
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

# 戻ったあと、映像の出力が戻ったか。worker の 1 秒ごとのログ (dec N fps, out N fps) のうち、戻した時刻
# (decoder: test: timestamps rewound) の 2 秒後から 4 秒間の行で、出力の fps が受信 (デコード) の fps の半分以上
check_output() {  # check_output <説明> <このケースの worker のログ>
    awk -v name="$1" '
        function secs(t,   a) { split(t, a, ":"); return a[1] * 3600 + a[2] * 60 + a[3] }
        /decoder: test: timestamps rewound/ { t0 = secs($2); rewound = 1 }
        rewound && match($0, /dec [0-9]+ fps, out [0-9]+ fps/) {
            t = secs($2) - t0
            if (t < 0) t += 86400
            if (t < 2 || t >= 6) next
            split(substr($0, RSTART, RLENGTH), w, " ")
            n++
            if (w[2] < 30 || w[5] * 2 < w[2]) bad++
            list = list sprintf(" %d/%d", w[5], w[2])
        }
        END {
            ok = rewound && n >= 3 && !bad
            printf "    output/decoded fps, 2-6 s after the rewind:%s\n", n ? list : " (no lines)"
            print (ok ? "OK   " : "FAIL ") name
            exit !ok
        }' "$2"
}

# このケースの worker のログを切り出す (ケースの前に log_mark、後に log_cut)
log_mark() { SKIP=$(cat "$CH_LOG" 2>/dev/null | wc -l); }
log_cut() {
    tail -n +"$((SKIP + 1))" "$CH_LOG" >"$OUT/rewind-case.log"
    cat "$OUT/rewind-case.log" >>"$RUN_LOG"
}

"$BIN/denpachan-engine.exe" --config "$CFG" --log-file "$LOG" >/dev/null 2>&1 &
sleep 2
"$CTL" set 1 "{\"debugArgs\":\"--test-ts-rewind 6\",\"record\":{\"enabled\":true,\"dir\":\"$(cygpath -w "$REC" | sed 's/\\/\\\\/g')\",\"template\":\"{ch} {hh}{mm}{ss}\",\"gapTimeoutSec\":5}}" >/dev/null

run_case() {  # run_case <説明> <record の JSON>
    echo "==== $1"
    touch "$MARK"  # このケースのファイルを見分ける
    sleep 1
    "$CTL" set 1 "{\"record\":$2}" >/dev/null
    log_mark
    "$CTL" start 1
    sleep 2
    send 14
    sleep 7  # 途切れ (5 秒) でファイルが閉じるまで
    "$CTL" stop 1
    sleep 1
    log_cut
    check_output "$1: 戻ったあとの映像の出力" "$OUT/rewind-case.log" || FAIL=1
    local f
    f=$(find "$REC" -type f -newer "$MARK" -name "*.mkv" | sort | tail -1)
    if [ -z "$f" ]; then echo "FAIL $1: no file"; FAIL=1; return; fi
    check_file "$1" "$f" || FAIL=1
}

run_case "パススルー" '{"mode":"passthrough","container":"mkv"}'
run_case "再エンコード ($ENCODER)" "{\"mode\":\"encode\",\"container\":\"mkv\",\"encode\":{\"video\":{\"codec\":\"h264\",\"encoder\":\"$ENCODER\"}}}"

# 途絶のあとに戻る (配信の始め直し)。録画はしない。タイムスタンプが負にならないよう、送出は 900 秒から始める
echo "==== 途絶 (3 秒) のあとに 600 秒戻る"
"$CTL" set 1 '{"debugArgs":"--test-ts-rewind 6 --test-ts-rewind-gap 3000 --test-ts-rewind-by 600","record":{"enabled":false}}' >/dev/null
log_mark
"$CTL" start 1
sleep 2
send 22 -bf 2 -output_ts_offset 900
"$CTL" stop 1
sleep 1
log_cut
check_output "途絶のあとに戻る: 戻ったあとの映像の出力" "$OUT/rewind-case.log" || FAIL=1

taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
echo
echo "==== worker log (this run)"
grep -E "test: |record:|re-anchor|off the current timeline|playout: (stalled|live)|timestamps jumped" "$RUN_LOG" | tail -30 | cut -c12-
exit $FAIL
