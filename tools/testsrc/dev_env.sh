# 試験スクリプトから読み込む (. "$ROOT/tools/testsrc/dev_env.sh")。
#
# FFmpeg は同梱しない (docs/dependencies.md) ので、ビルド出力には FFmpeg の DLL が無い。
# 試験では、ビルドのときに cmake/FetchFfmpeg.cmake が取ってきた FFmpeg の shared 版 (利用者が使うのと同じ配布物) の
# bin を PATH の先頭に足して使う。録画の CPU エンコードもこの bin の ffmpeg.exe (gpl 版、x264 入り) を使う。
# FFmpeg が無いときの動作は ffmpeg_missing_test.sh で確かめる。
FFMPEG_DEV_BIN="$ROOT/out/deps/ffmpeg-9.0.2-full_build-shared/bin"
export PATH="$FFMPEG_DEV_BIN:$PATH"

# 画面 (denpachan.exe) が動いている間は試験を回さない。試験は engine を名前で止めて、試験用の設定で起動し直すが、
# 画面は engine がいなくなると普段の設定で起動し直す。すると試験用の engine は起動できず、試験の ctl の操作
# (設定の変更、開始・停止) が、普段の設定 (%APPDATA%\denpachan\config.json) に入ってしまう
if tasklist //FI "IMAGENAME eq denpachan.exe" //NH 2>/dev/null | grep -qi "^denpachan\.exe"; then
    echo "denpachan の画面 (denpachan.exe) が動いている。閉じてから試験を回す (engine を止めても、画面が普段の設定で起動し直す)" >&2
    exit 1
fi
