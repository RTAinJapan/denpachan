# 試験スクリプトから読み込む (. "$ROOT/tools/testsrc/dev_env.sh")。
#
# FFmpeg は同梱しない (docs/dependencies.md) ので、ビルド出力には FFmpeg の DLL が無い。
# 試験では、ビルドのときに cmake/FetchFfmpeg.cmake が取ってきた FFmpeg の shared 版 (利用者が使うのと同じ配布物) の
# bin を PATH の先頭に足して使う。録画の CPU エンコードもこの bin の ffmpeg.exe (gpl 版、x264 入り) を使う。
# FFmpeg が無いときの動作は ffmpeg_missing_test.sh で確かめる。
FFMPEG_DEV_BIN="$ROOT/out/deps/ffmpeg-9.0.2-full_build-shared/bin"
export PATH="$FFMPEG_DEV_BIN:$PATH"
