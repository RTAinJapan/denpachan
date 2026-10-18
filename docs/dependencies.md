# 依存関係

denpachan 本体は MIT ライセンス。ライセンス上の制約が付かないように、**FFmpeg は同梱しない**。
利用者が FFmpeg の shared 版を用意し、denpachan はその DLL を実行時に読み込む。

## 1. 利用者が用意するもの

| もの | 必須 | 用途・入手先 |
|------|------|-------------|
| Windows 10 / 11 (x64) | 必須 | |
| Microsoft Visual C++ 再頒布可能パッケージ (x64、最新版) | 必須 | engine / worker / rtmpd / ctl が使う (仮想カメラの DLL は CRT を静的リンクしているので不要)。VS 2026 でビルドしているので、古い版では動かないことがある。https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist |
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
| Visual Studio (MSVC、C++ によるデスクトップ開発) | 2026 (MSVC 14.5x)。CI の windows-latest にも入っている | |
| CMake / Ninja | CMake 3.25 以上 | Visual Studio に付属のもの |
| vcpkg | baseline `bf19794383e8fb95eba5aa553235bc8ec2271688` | `vcpkg.json` (libsrt、spdlog、nlohmann-json。FFmpeg は入れない) |
| .NET 10 SDK | | 画面 |
| FFmpeg の shared 版 (gyan.dev) | 9.0.2 | **ヘッダと .def だけ**を使う (§3.1)。配布物には入れない |

### 3.1 FFmpeg (ビルド用)

- `cmake/FetchFfmpeg.cmake` が、利用者に案内するのと同じ配布物 [ffmpeg-9.0.2-full_build-shared.zip](https://github.com/GyanD/codexffmpeg/releases/tag/9.0.2) (gyan.dev の GitHub ミラー、約 95 MB) を固定の URL と SHA256 で取り、`out/deps/` に展開する。
  別の場所に展開済みのものを使うときは `-DDENPACHAN_FFMPEG_DIR=<フォルダ>`。
- 使うのは `include/` のヘッダと、`lib/*.def` から MSVC の `lib.exe` で作り直した import lib (`out/build/ffmpeg-implib/`)。
  配布物の `.lib` は MinGW のツールで作られていて、MSVC の `/DELAYLOAD` が効かない (通常の読み込みになって、FFmpeg が無いと起動できなくなる) ので使わない。
- FFmpeg の DLL は遅延ロード (`/DELAYLOAD`) にしてあり、worker と engine は使う前に有無と版を確かめる (`src/common/include/denpachan/ffmpeg_runtime.h`)。
  FFmpeg を新しい major に上げたら、`FetchFfmpeg.cmake` の版と SHA256、`ffmpeg_runtime.h` の `kSeries` / `kAvcodecMajor` を変える (変えないとビルドが止まる)。
- ビルド出力に FFmpeg の DLL があれば消す (`cmake/FfmpegRuntime.cmake`。配布物に紛れ込まないように)。
- 開発中に動かすとき: `out/deps/ffmpeg-9.0.2-full_build-shared/bin` を PATH に足す (利用者と同じ配布版の DLL と ffmpeg.exe)。
  試験スクリプトは `tools/testsrc/dev_env.sh` でこれを行う。FFmpeg が無いときの動作は `tools/testsrc/ffmpeg_missing_test.sh` で確かめる。

### 3.2 CI (GitHub Actions)

`.github/workflows/build.yml`。push と pull request でビルドする。

- vcpkg を baseline と同じコミットで取り、`build.ps1` でビルドする。vcpkg のバイナリと、取った FFmpeg は Actions のキャッシュに入れる。
- ビルド後に確かめること: 出力に FFmpeg の DLL が無い、必要なファイルとライセンス文がある、FFmpeg が無い状態で worker は専用の終了コード (0x0FF0) で終わり、engine は起動して「FFmpeg が見つかりません」をログに出す。
- artifact: `denpachan-win64` (配布物。FFmpeg を含まない) と `denpachan-win64-symbols` (PDB)。

### 3.3 配布の形

- 配布は zip (`tools/package.ps1` が `out/dist/denpachan-<版>-win64.zip` を作る。CI でも同じものを artifact にする)。展開すればインストールせずに使える。
  - 中身: 実行ファイルと同梱の DLL、`licenses/`、README.md、docs/ (設計・依存関係・ライセンス)。PDB は別の zip。開発用の audio_probe.exe は入れない。FFmpeg が入っていたら作るのを止める。
  - 仮想カメラは画面が起動時にこのユーザーに登録する (設計書 §5.1)。
- .NET 10 デスクトップ ランタイム、VC++ 再頒布可能パッケージ、FFmpeg 9.x shared は同梱せず、README に入手先を書く。
- 展開した zip での動作は `tools/testsrc/package_test.sh` で確かめる (仮想カメラの登録と、DirectShow で映像が取れること)。
