#!/usr/bin/env bash
# 配布用の zip を展開してすぐ使えるかの試験 (tools/package.ps1 のあとに実行する)。
#
# 1. out/dist の zip を新しいフォルダに展開する (FFmpeg は PATH に置く。利用者と同じ形)
# 2. 展開したフォルダの denpachan.exe を起動 → 仮想カメラがこのフォルダの DLL で登録し直される
# 3. ch1 に SRT を送り、DirectShow (ffmpeg の dshow 入力。OBS と同じ経路) で denpachan Cam 1 から映像を取れる
# 4. 片付け: 仮想カメラの登録を out/build に戻す
#
# 普段の設定 (%APPDATA%\denpachan\config.json) を使わないよう、試験用の設定で engine を先に起動しておく
# (画面は engine が動いていれば起動しない)。
#
#   tools/testsrc/package_test.sh
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tools/testsrc/dev_env.sh"
OUT="$ROOT/out/check"
DEST="$OUT/package-test"
CFG="$OUT/package-config.json"
DEV_BIN="$ROOT/out/build/bin/Release"
ZIP=$(ls -t "$ROOT"/out/dist/*-win64.zip 2>/dev/null | grep -v symbols | head -1)
[ -n "$ZIP" ] || { echo "FAIL: run tools/package.ps1 first"; exit 1; }
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1
sleep 1
rm -rf "$DEST" "$CFG"
mkdir -p "$DEST"

step() { echo; echo "==== $*"; }
vcam_path() {
    powershell.exe -NoProfile -Command "(Get-ItemProperty 'HKCU:\Software\Classes\CLSID\{E952D5CE-ACFF-40F2-836E-000EAA95AC9E}\InprocServer32' -ErrorAction SilentlyContinue).'(default)'" | tr -d '\r'
}

step "1. zip を展開: $(basename "$ZIP")"
unzip -q "$ZIP" -d "$DEST"
APP=$(ls -d "$DEST"/denpachan-*)
ls "$APP" | tr '\n' ' '; echo

step "2. 展開したフォルダの denpachan.exe を起動 → 仮想カメラをこのフォルダで登録"
echo "before: $(vcam_path)"
"$APP/denpachan-engine.exe" --config "$CFG" --log-file "$OUT/package-engine.log" >/dev/null 2>&1 &
sleep 2
"$APP/denpachan.exe" --screenshot "$OUT/ui-package.png" --delay 4
AFTER=$(vcam_path)
echo "after:  $AFTER"
case "$AFTER" in
    "$(cygpath -w "$APP")\\denpachan-vcam.dll") echo "OK: registered from the extracted folder" ;;
    *) echo "FAIL: not registered from the extracted folder" ;;
esac

step "3. ch1 に SRT を送り、DirectShow で denpachan Cam 1 から映像を取る"
"$APP/denpachan-ctl.exe" start 1
ffmpeg -hide_banner -loglevel error -re -f lavfi -i "testsrc2=size=1280x720:rate=60" \
    -c:v libx264 -preset veryfast -tune zerolatency -b:v 4M -g 60 -bf 0 -pix_fmt yuv420p -t 15 \
    -f mpegts "srt://127.0.0.1:9001?mode=caller&latency=120000&pkt_size=1316" >/dev/null 2>&1 &
SENDER=$!
sleep 4
"$APP/denpachan-ctl.exe" summary | head -1
ffmpeg -hide_banner -f dshow -list_devices true -i dummy 2>&1 | grep -o '"denpachan Cam [0-9]"' | tr '\n' ' '; echo
YAVG=$(ffmpeg -hide_banner -f dshow -i video="denpachan Cam 1" -frames:v 30 -vf signalstats,metadata=print:key=lavfi.signalstats.YAVG -f null - 2>&1 |
    grep -o 'YAVG=[0-9.]*' | tail -1)
echo "cam1 $YAVG"
case "$YAVG" in
    YAVG=1[0-9][0-9]*) echo "OK: picture from denpachan Cam 1" ;;
    *) echo "FAIL: no picture from denpachan Cam 1" ;;
esac

step "4. 片付け (仮想カメラの登録を out/build に戻す)"
kill "$SENDER" 2>/dev/null
"$APP/denpachan-ctl.exe" stop 1
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
sleep 1
"$DEV_BIN/denpachan-engine.exe" --config "$CFG" --log-file "$OUT/package-engine.log" >/dev/null 2>&1 &
sleep 2
"$DEV_BIN/denpachan.exe" --screenshot "$OUT/ui-package-restore.png" --delay 3
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
echo "restored: $(vcam_path)"
