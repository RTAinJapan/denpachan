# denpachan

SRT / RTMP で受けた映像と音声 (最大 4 チャンネル) を、OBS などから使える仮想カメラと、任意の音声デバイス (Dante Virtual Soundcard など) に低遅延で出す Windows 用のソフトウェア。録画もできる。

## 使い方

配布の zip (`denpachan-<版>-win64.zip`) を展開すれば、インストールせずに使える。

### 1. 先に入れておくもの

| もの | 入手先 |
|------|--------|
| Windows 10 / 11 (x64) | |
| .NET 10 デスクトップ ランタイム (x64) | https://dotnet.microsoft.com/download/dotnet/10.0 の「.NET Desktop Runtime」 |
| Visual C++ 再頒布可能パッケージ (x64、最新版) | https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist |
| **FFmpeg 9.x** | [gyan.dev](https://www.gyan.dev/ffmpeg/builds/) の「release full shared」、または [BtbN](https://github.com/BtbN/FFmpeg-Builds/releases) の `win64-gpl-shared-9.0` (どちらか 1 つ) |

- FFmpeg は、展開した `bin` フォルダを **PATH に通す**か、`bin` の中身を **denpachan と同じフォルダに置く**。
  見つからないときは、画面の上部に赤い帯で案内が出る (入れれば再起動しなくても動き出す)。
- あると良いもの: GPU のドライバ (ハードウェアデコード・録画の GPU エンコード)、OBS など DirectShow の映像を取り込めるソフト、Dante Virtual Soundcard など。

### 2. 起動する

1. zip を好きな場所に展開する。
   ダウンロードした zip は、展開する前にファイルのプロパティで「許可する」にチェックしておくと、Windows の警告 (SmartScreen) が出にくい。
   警告が出たら「詳細情報」→「実行」。
2. `denpachan.exe` を起動する。受信を担当する engine は自動で起動する。
   仮想カメラ (denpachan Cam 1〜4) は、初めて起動したときに**このユーザーに自動で登録**される (管理者権限は要らない。フォルダを移したときも登録し直す)。
3. チャンネルの「設定…」で入力 (SRT / RTMP) を決めて「開始」。
   初めて受信を始めるときに Windows ファイアウォールの確認が出たら、許可する (SRT は UDP の各チャンネルのポート、RTMP は TCP 1935)。
4. OBS では「ソース」→「映像キャプチャデバイス」→ `denpachan Cam 1`〜`4` を選ぶ。

- 画面を閉じると受信も止まる (仮想カメラと音声の出力も止まる)。受信中のチャンネルがあるときは、閉じてよいかを確認する。本番中は画面を閉じないこと。
- 画面が落ちたときは、受信は止まらずに続く。もう一度 `denpachan.exe` を起動すれば、動いている受信につながる。
- 設定は `%APPDATA%\denpachan`、ログとクラッシュダンプは `%LOCALAPPDATA%\denpachan` に置かれる。

### 3. アンインストール

1. メニューの「仮想カメラの登録を解除」を選ぶ。
2. 画面を閉じる。OBS や Discord など、カメラを使うアプリも閉じる (denpachan Cam を使ったままだと、フォルダを消せない)。
3. denpachan のフォルダを消す。設定とログも消すなら `%APPDATA%\denpachan` と `%LOCALAPPDATA%\denpachan` も消す。

## ドキュメント

- 設計: [docs/design.md](docs/design.md)
- 依存関係・ライセンス: [docs/dependencies.md](docs/dependencies.md)

## ビルド

```powershell
./build.ps1
```

Visual Studio 2026 (C++ によるデスクトップ開発)、.NET 10 SDK、vcpkg (`VCPKG_ROOT`) が必要。出力は `out/build/bin/Release`。

- ビルドに使う FFmpeg のヘッダは、初回の configure で FFmpeg 9.0.2 の shared 版を `out/deps/` に取ってくる (配布物には入れない)。
- FFmpeg の DLL は出力に入らないので、開発中は `out/deps/ffmpeg-9.0.2-full_build-shared/bin` を PATH に足して動かす。
- GitHub Actions でも同じようにビルドし、配布用の zip を artifact にする (`.github/workflows/build.yml`)。

### 画面の文言を直すとき (VS Code)

engine や配信がなくても、受信中の状態の画面を出して、文言を直しながら確かめられる。

1. VS Code でこのフォルダを開く (おすすめの拡張機能は `.vscode/extensions.json`)。
2. 「ターミナル」→「タスクの実行」→「画面: 文言を確認 (メイン画面)」を選ぶ (流し込む状態は normal / degraded / ffmpeg-missing / disconnected から選ぶ)。チャンネル設定・録画タブ・全体設定のタスクもある。
3. `src/ui/Denpachan.Ui` の XAML や C# を直して保存すると、数秒でビルドし直して画面が起動し直す (`dotnet watch`)。止めるときはターミナルで Ctrl+C。

コマンドで動かすときは次のとおり (`--settings 1` / `--settings 1 --settings-tab 1` / `--general` で設定画面も開く)。

```powershell
dotnet watch --no-hot-reload run --project src/ui/Denpachan.Ui/Denpachan.Ui.csproj -- --fixture tools/ui_screenshots/cases/normal.json
```

- 画面に流し込む状態は `tools/ui_screenshots/cases/*.json` (スクリーンショット試験と同じもの)。
- XAML はホットリロードが効かないので、ホットリロードを切って毎回起動し直している。

## ライセンス

MIT ([LICENSE](LICENSE))。同梱しているライブラリのライセンスは [docs/third_party_notices.md](docs/third_party_notices.md) (配布物では `licenses/`)。
