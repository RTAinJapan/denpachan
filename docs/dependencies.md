# 依存関係

denpachan 本体は MIT ライセンス。ライセンス上の制約が付かないように、**FFmpeg は同梱しない**。
利用者が FFmpeg の shared 版を用意し、denpachan はその DLL を実行時に読み込む。

## 1. 利用者が用意するもの

| もの | 必須 | 用途・入手先 |
|------|------|-------------|
| Windows 10 / 11 (x64) | 必須 | |
| Microsoft Visual C++ 再頒布可能パッケージ (x64, 2015-2022 以降) | 必須 | engine / worker / rtmpd / ctl が使う (仮想カメラの DLL は CRT を静的リンクしているので不要)。多くの PC には入っている (インストーラを作るときに同梱を検討する) |
| .NET 10 デスクトップ ランタイム | 必須 | 画面 (denpachan.exe) |
| **FFmpeg 9.x の shared 版** | 必須 | 受信した映像・音声のデコード、録画、48 kHz 以外の音声デバイスへのリサンプル。下記 §1.1 |
| GPU のドライバ | 推奨 | ハードウェアデコード、録画の GPU エンコード (NVIDIA: NVDEC / NVENC、Intel: QSV、AMD: AMF) |
| OBS など DirectShow の映像を取り込めるソフト | 用途による | 仮想カメラ (denpachan Cam 1〜4) の取り込み |
| Dante Virtual Soundcard など | 用途による | 音声の出力先 |

### 1.1 FFmpeg

- **FFmpeg 9.x の shared 版** (DLL が入っているもの) を使う。static 版 (ffmpeg.exe 1 つだけのもの) では動かない。
  - 例: [gyan.dev](https://www.gyan.dev/ffmpeg/builds/) の「release full shared」、[BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases) の `win64-gpl-shared-9.0` / `win64-lgpl-shared-9.0`
- 展開した `bin` フォルダを **PATH に通す**か、中の DLL を **denpachan と同じフォルダに置く**。
  使う DLL: `avcodec-63.dll`, `avformat-63.dll`, `avutil-61.dll`, `swresample-7.dll`, `swscale-10.dll` (とそれらが使う DLL)。
- 版の決まり: denpachan をビルドした FFmpeg と major が同じ (avcodec 63 = FFmpeg 9 系) で、minor が同じか新しいこと。
  9.0.x 同士は互換。FFmpeg 10 (avcodec 64) では動かないので、denpachan 側を対応させる。
- 見つからない・版が合わないとき: engine と画面は動き、画面の上部とチャンネルの状態に「FFmpeg 9.x (shared 版) が見つかりません」を出す。
  受信 (worker) は起動しない。engine は 10 秒ごとに探し直し、見つかれば再起動なしで受信を始める。
- 録画の **CPU エンコード** (x264 / x265 / SVT-AV1) は、FFmpeg の `ffmpeg.exe` を子プロセスとして起動して行う (設計書 §8.4)。
  shared 版の `bin` には ffmpeg.exe も入っているので、PATH に通せば追加の用意は要らない。x264 / x265 を使うには **gpl** 版が必要 (lgpl 版の ffmpeg.exe には入っていない)。
  全体の設定で別の ffmpeg.exe を指定することもできる。
- GPU エンコード (NVENC / QSV / AMF) は、読み込んだ FFmpeg の DLL に入っているものを使う。上記の配布版にはどれも入っている。

### 1.2 なぜ FFmpeg を同梱しないか

- FFmpeg 自体は LGPL (x264 などを含めると GPL)。同梱すると、配布物にそのライセンスの義務 (ソースの提供など) が付く。
- 利用者が自分で入れた DLL を読み込む形なら、denpachan の配布物には FFmpeg が含まれない。
- GPL のエンコーダ (x264 / x265) は、利用者の ffmpeg.exe を別のプロセスとして呼び出すだけにする (denpachan のプロセスには読み込まない)。
- H.264 / HEVC などの特許ライセンスは、これとは別の話 (GPU エンコーダでも同じ)。

## 2. 同梱するもの

配布物 (ビルド出力の `bin/Release`) に入る、denpachan 以外のもの。ライセンス文は `bin/Release/licenses/` にコピーされる (`docs/third_party_notices.md`)。

| ファイル | ソフトウェア | 版 | ライセンス | 使うもの |
|---------|-------------|----|-----------|---------|
| `srt.dll` | SRT | 1.5.7 | MPL-2.0 | worker |
| `libcrypto-3-x64.dll` | OpenSSL | 3.6.5 | Apache-2.0 | srt.dll (暗号化) |
| `spdlog.dll`, `fmt.dll` | spdlog / {fmt} | 1.17.0 / 12.2.0 | MIT | engine / worker / rtmpd |
| (組み込み) | nlohmann/json | 3.12.0 | MIT | engine / worker / ctl |

- SRT は MPL-2.0 なので、ソースの入手先 (上流のタグと、ビルドに使った vcpkg のポート) を `third_party_notices.md` に書いている。
- 仮想カメラの DLL (`denpachan-vcam.dll`) は外部のライブラリを使わない (DirectShow の基底クラスも使わず、CRT は静的リンク)。
- 画面 (C# / WPF) は NuGet のパッケージを使っていない。

## 3. ビルドに使うもの (配布しない)

| もの | 版 | 備考 |
|------|----|------|
| Visual Studio 2026 (MSVC v14.5、C++ によるデスクトップ開発) | | |
| CMake / Ninja | CMake 3.25 以上 | Visual Studio に付属のもの |
| vcpkg | baseline `bf19794383e8fb95eba5aa553235bc8ec2271688` | `vcpkg.json` |
| .NET 10 SDK | | 画面 |
| FFmpeg (vcpkg) | 9.0.2 | **ヘッダと import lib だけ**を使う。DLL はビルド後に出力から消す (`cmake/FfmpegRuntime.cmake`)。NVENC (ffnvcodec のヘッダ、MIT) と QSV (libvpl、MIT) を有効にしている |

- FFmpeg の DLL は遅延ロード (`/DELAYLOAD`) にしてあり、worker と engine は使う前に有無と版を確かめる (`src/common/include/denpachan/ffmpeg_runtime.h`)。
  FFmpeg のヘッダを新しい major に上げたら、`ffmpeg_runtime.h` の `kSeries` / `kAvcodecMajor` を変える (変えないとビルドが止まる)。
- 開発中に動かすとき: `out/build/vcpkg_installed/x64-windows/bin` (vcpkg がビルドした FFmpeg 9.0.2 の DLL) を PATH に足す。
  試験スクリプトは `tools/testsrc/dev_env.sh` でこれを行う。FFmpeg が無いときの動作は `tools/testsrc/ffmpeg_missing_test.sh` で確かめる。

### 3.1 今後の検討

- CI (GitHub Actions) でビルドするとき、FFmpeg をソースからビルドする代わりに、配布されている shared 版の `include/` と `lib/` を落として使えば速い。
  そうするなら `vcpkg.json` から ffmpeg を外す。どちらにしても配布物には入れない。
- インストーラ (M7) では VC++ 再頒布可能パッケージと .NET ランタイムの扱いを決める。FFmpeg はインストーラでも同梱せず、入手先を案内する。
