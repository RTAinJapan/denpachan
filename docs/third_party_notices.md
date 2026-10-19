# 同梱しているソフトウェアとライセンス

denpachan は MIT License で配布しています (`denpachan.txt`)。Copyright (c) 2026 RTA in Japan。

ただし、ASIO 出力の `denpachan-asio.dll` だけは、Steinberg の ASIO SDK を GPL-3.0 で使っているため、
GNU General Public License v3.0 (GPL-3.0) です (`denpachan-asio.txt`)。この DLL を含むこの配布物は、全体として GPL-3.0 の条件で配布しています。
ソースコードは https://github.com/RTAinJapan/denpachan で入手できます (この版のタグ)。
`denpachan-asio.dll` を消すと、ASIO の出力先が出なくなるだけで、他はそのまま動きます。

配布物には次のライブラリを同梱・組み込みしています。各ライセンス文はこのフォルダにあります。

| ファイル | ソフトウェア | 版 | ライセンス | ライセンス文 |
|---------|-------------|----|-----------|-------------|
| `srt.dll` | SRT (Haivision) | 1.5.7 | MPL-2.0 | `libsrt.txt` |
| `libcrypto-3-x64.dll` | OpenSSL (SRT の暗号化に使用) | 3.6.5 | Apache-2.0 | `openssl.txt` |
| `spdlog.dll` | spdlog | 1.17.0 | MIT | `spdlog.txt` |
| `fmt.dll` | {fmt} | 12.2.0 | MIT | `fmt.txt` |
| (実行ファイルに組み込み) | JSON for Modern C++ (nlohmann/json) | 3.12.0 | MIT | `nlohmann-json.txt` |
| (`denpachan-asio.dll` に組み込み) | ASIO SDK (Steinberg Media Technologies GmbH) | 2.3.4 | GPL-3.0 | `asiosdk.txt` |

ASIO is a trademark and software of Steinberg Media Technologies GmbH.

## SRT (MPL-2.0) のソースコード

`srt.dll` は SRT 1.5.7 を vcpkg でビルドしたものです。ソースコードは次から入手できます。

- 上流のソース: https://github.com/Haivision/srt/tree/v1.5.7
- ビルドに使った vcpkg のポート (ビルド設定用のパッチ 3 つを含む):
  https://github.com/microsoft/vcpkg/tree/bf19794383e8fb95eba5aa553235bc8ec2271688/ports/libsrt

denpachan は SRT のソースを変更していません。

## FFmpeg について

FFmpeg は**同梱していません**。利用者が FFmpeg 9.x の shared 版を用意して使います (詳しくは denpachan の `docs/dependencies.md`)。
録画の CPU エンコードで使う ffmpeg.exe (x264 / x265 など) も、利用者が用意したものを別のプロセスとして起動します。
