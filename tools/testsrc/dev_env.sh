# 試験スクリプトから読み込む (. "$ROOT/tools/testsrc/dev_env.sh")。
#
# FFmpeg は同梱しない (docs/dependencies.md) ので、ビルド出力には FFmpeg の DLL が無い。
# 試験では vcpkg がビルドした FFmpeg の DLL (ビルドに使ったのと同じ版) を PATH の先頭に足して使う。
# FFmpeg が無いときの動作は ffmpeg_missing_test.sh で確かめる。
FFMPEG_DEV_BIN="$ROOT/out/build/vcpkg_installed/x64-windows/bin"
export PATH="$FFMPEG_DEV_BIN:$PATH"
