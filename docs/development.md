# 開発

denpachan をビルド・試験・修正する人向けの手順。設計は [design.md](design.md)、依存関係とライセンスは [dependencies.md](dependencies.md)。

## ビルド

```powershell
./build.ps1
```

Visual Studio 2026 (C++ によるデスクトップ開発)、.NET 10 SDK、vcpkg (`VCPKG_ROOT`) が必要。出力は `out/build/bin/Release`。

- ビルドに使う FFmpeg のヘッダは、初回の configure で FFmpeg 9.0.2 の shared 版を `out/deps/` に取ってくる (配布物には入れない)。
- FFmpeg の DLL は出力に入らないので、開発中は `out/deps/ffmpeg-9.0.2-full_build-shared/bin` を PATH に足して動かす。
- 配布用の zip は `tools/package.ps1` で作る (`out/dist/`)。
- GitHub Actions でも同じようにビルドし、配布用の zip を artifact にする (`.github/workflows/build.yml`)。

## 試験

`tools/testsrc/` のスクリプトで、実際に送出 (FFmpeg) して engine / worker を動かして確かめる。Git Bash から実行する。
FFmpeg の DLL は `tools/testsrc/dev_env.sh` が PATH に足す。普段の設定は使わず、`out/check/` に試験用の設定とログを置く。

| スクリプト | 確かめること |
|-----------|-------------|
| `engine_test.sh` | worker の監視・再起動、engine を落としたときの引き継ぎ、設定の検証 |
| `rtmp_test.sh` | RTMP の push (キーの振り分け、二重 publish、rtmpd の再起動) と pull |
| `hold_test.sh` | 途絶えたときに最後の映像を保持する、NO SIGNAL の出し方 |
| `record_test.sh` | 録画 (パススルー / GPU / CPU、区切り、再接続時の継続、ファイル名) |
| `ffmpeg_missing_test.sh` | FFmpeg が無いときの動きと、置いたあとの自動復帰 |
| `package_test.sh` | 配布用の zip を展開して起動 (仮想カメラの登録、DirectShow での取り込み) |
| `close_test.ps1` | 画面を閉じたときの確認ダイアログと、受信の停止 |
| `pull_test.sh <url>` | 実際のサーバーからの RTMP pull / SRT caller の受信状況 |

画面の見た目は `tools/ui_screenshots/run.ps1` (engine なしで、試験用の状態を流し込んでスクリーンショットを撮る。CI でも動く)。

## 画面の文言を直すとき (VS Code)

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
- C++ のビルドは `build.ps1` (タスク「ビルド (全体)」) で行う。CMake Tools が開いたときに勝手に configure しないよう、`.vscode/settings.json` で止めている (同じ `out/build` を触って `build.ps1` とぶつかるため)。

## アイコン

元は `assets/denpachan.svg`。これを直したら次を実行して `assets/denpachan.ico` を作り直し、両方をコミットする。

```powershell
./tools/icon/make_icon.ps1
```

- SVG を Edge (ヘッドレス) で描いて、16〜256 px を 1 つの ico にまとめる。ビルドには ico だけを使う (Edge は要らない)。
- 縮めた各サイズの PNG は `out/icon/` に残るので、小さいサイズで潰れていないかをそこで見る。
- 画面 (`denpachan.exe`) は csproj の `ApplicationIcon`、engine / worker / rtmpd / ctl は `src/common/icon.rc.in` で ico を埋め込む。ウィンドウとタスクバーも exe のアイコンを使う。
