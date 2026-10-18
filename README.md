# denpachan

SRT / RTMP で受けた映像と音声 (最大 4 チャンネル) を、OBS などから使える仮想カメラと、任意の音声デバイス (Dante Virtual Soundcard など) に低遅延で出す Windows 用のソフトウェア。録画もできる。

- 設計: [docs/design.md](docs/design.md)
- 依存関係・ライセンス: [docs/dependencies.md](docs/dependencies.md)

## 動かすのに必要なもの

- Windows 10 / 11 (x64)、Visual C++ 再頒布可能パッケージ (x64)、.NET 10 デスクトップ ランタイム
- **FFmpeg 9.x の shared 版** (同梱していない)。`bin` フォルダを PATH に通すか、DLL を denpachan と同じフォルダに置く。
  録画を CPU (x264 / x265) でエンコードするときは gpl 版の ffmpeg.exe も使う。詳しくは [docs/dependencies.md](docs/dependencies.md)。

## ビルド

```powershell
./build.ps1
```

Visual Studio 2026 (C++ によるデスクトップ開発)、.NET 10 SDK、vcpkg (`VCPKG_ROOT`) が必要。出力は `out/build/bin/Release`。
FFmpeg の DLL は出力に入らないので、開発中は `out/build/vcpkg_installed/x64-windows/bin` を PATH に足して動かす。

## ライセンス

MIT ([LICENSE](LICENSE))。同梱しているライブラリのライセンスは [docs/third_party_notices.md](docs/third_party_notices.md)。
