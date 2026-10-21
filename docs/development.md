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
- GitHub Actions でも同じようにビルドし、配布用の zip を artifact にする (`.github/workflows/build.yml`)。push・PR のたびに動く。
- 好きなブランチを手動でビルドするときは、GitHub の Actions → build → 「Run workflow」でブランチを選ぶ。zip は実行結果の artifact から取れる。

## リリース

版は [セマンティック バージョニング](https://semver.org/lang/ja/) の `X.Y.Z` で数える。

- X (メジャー): 前の版の設定ファイルが読めなくなるなど、入れ替えるだけでは済まない変更
- Y (マイナー): 機能の追加
- Z (パッチ): 不具合の修正だけ
- 試してもらうための版は、後ろにラベルを付ける (`1.2.0-rc.1`、`1.2.0-beta` など)。ラベル付きはプレリリースになる。

版を書くのは `CMakeLists.txt` の `project(denpachan ... VERSION X.Y.Z)` の 1 か所だけ (画面の csproj もここを読む。ラベルはここには書かない)。

- 版は、画面のタイトル (`denpachan X.Y.Z`) と、exe・DLL のプロパティの「詳細」(`cmake/VersionResource.cmake`) に出る。
- ラベルは `./build.ps1 -VersionLabel rc.1` で付く (CI はタグ `vX.Y.Z-rc.1` のときに付ける)。画面のタイトルと製品バージョンが `X.Y.Z-rc.1` になる (ファイルバージョンは数字だけなので `X.Y.Z.0` のまま)。

手順:

1. `CMakeLists.txt` の版を上げて main にコミットする (PR でよい)。
2. そのコミットにタグ `vX.Y.Z` (プレリリースなら `vX.Y.Z-rc.1` など) を付けて push する。

   ```bash
   git tag v1.2.0
   ```

   ```bash
   git push origin v1.2.0
   ```

3. GitHub Actions がタグでビルドし、GitHub Release「denpachan X.Y.Z」を作って `denpachan-X.Y.Z-win64.zip` と PDB の zip を付ける。リリースノートは前のタグからの PR とコミットで GitHub が作るので、必要なら Release の画面で書き足す。

タグの形が違う、またはタグの版が `CMakeLists.txt` と合わないときは、ビルドの最初で失敗し、リリースは作られない。そのときはタグを消して付け直す (`git push origin :refs/tags/v1.2.0` で GitHub のタグを消す)。

## 試験

`tools/testsrc/` のスクリプトで、実際に送出 (FFmpeg) して engine / worker を動かして確かめる。Git Bash から実行する。
FFmpeg の DLL は `tools/testsrc/dev_env.sh` が PATH に足す。普段の設定は使わず、`out/check/` に試験用の設定とログを置く。

| スクリプト | 確かめること |
|-----------|-------------|
| `engine_test.sh` | worker の監視・再起動、engine を落としたときの引き継ぎ、設定の検証 |
| `config_upgrade_test.sh` | 0.1.x (4ch) の設定の引き継ぎ (ch5〜8 の既定値とぶつかっても捨てない、チャンネル数を増やせる) |
| `rtmp_test.sh` | RTMP の push (キーの振り分け、二重 publish、rtmpd の再起動) と pull |
| `hold_test.sh` | 途絶えたときに最後の映像を保持する、NO SIGNAL の出し方 |
| `vcam_pace_test.sh` | 仮想カメラ (60 fps) が 30 fps / 60 fps の映像を受けたときに、同じ絵を余分に挟まないこと |
| `burst_test.sh` | 送出側が短く止まってからまとめて送ったときに、増えた遅延が数秒で戻ること (VB-Audio Virtual Cable を使う) |
| `record_test.sh` | 録画 (パススルー / GPU / CPU、区切り、再接続時の継続、ファイル名) |
| `rewind_test.sh` | 録画中に同じ接続のままタイムスタンプが戻ったとき (worker の `--test-ts-rewind`) に、ファイルが壊れず音声も続くこと |
| `ffmpeg_missing_test.sh` | FFmpeg が無いときの動きと、置いたあとの自動復帰 |
| `package_test.sh` | 配布用の zip を展開して起動 (仮想カメラの登録、DirectShow での取り込み) |
| `close_test.ps1` | 画面を閉じたときの確認ダイアログと、受信の停止 |
| `pull_test.sh <url>` | 実際のサーバーからの RTMP pull / SRT caller の受信状況 |

画面の見た目は `tools/ui_screenshots/run.ps1` (engine なしで、試験用の状態を流し込んでスクリーンショットを撮る。CI でも動く)。

## 画面の文言

画面に出る文は、すべて言語ファイル `src/ui/Denpachan.Ui/Lang/ja.json` (基準) と `en.json` にある (キーと文の組)。文言を直すときはこれらだけを直す。キーを足したら両方に足す (en.json に無いキーは英語の画面で日本語のまま出る)。英語の画面は `./tools/ui_screenshots/run.ps1 -Language en` で撮って見られる。

- キーは「画面.場所.もの」(例: `main.menu.file`、`channel.port`)。XAML では `{l:Tr main.menu.file}`、C# では `Loc.T("...")`。
- 文の中の `{name}` は引数で置き換わる (`Loc.F("event.connected", ("peer", peer))`)。数値の書式は C# 側で整えて渡す。
- `msg.` で始まるキーは worker / engine (C++) からのメッセージ。C++ は文を作らず `ui_message("net.dns", {{"host", host}})` で ID と引数を送り、画面が `msg.net.dns` で文にする (`src/common/include/denpachan/ui_message.h`)。
- キーを足したり消したりしたら `./tools/lang/check.ps1` で、コードが使うキーと ja.json が食い違っていないかを確かめる (CI でも実行)。
- 言語を足すときは ja.json を写して `Lang/<言語コード>.json` (例: `en.json`) を作り、`"_name"` をその言語での名前にして訳す。訳していないキーは日本語で出る。全体設定の「言語」に自動で並ぶ。

### 文の作り方の決まり

言語が変わると、語順も句読点も単語の形も変わる。日本語で自然に見える組み立て方でも、ほかの言語では文にならないことがある。次を守る。

- **1 つのキーは、それだけで完結した文 (またはラベル) にする。** 文の断片を作って C# で後ろにつながない。空白や「。」「、」で始まる文は断片のしるしで、`check.ps1` が弾く。
- **場合によって文の一部が変わるときは、場合ごとに文全体のキーを分ける。** 例: 推奨値があるときだけ「(推奨 …)」が付く行は、`main.tile.srtLine` と `main.tile.srtLineWithRecommend` の 2 つにする。
- **2 つの文をつなぐときは、つなぎ方もキーにする。** 例: `srtAdvice.withNote` = `{value}。{note}`。いくつかの語を並べるときは `common.listSeparator` でつなぐ。
- **文の中で変わる値は `{name}` の引数にする。** 語順は言語ファイルの側で決まる (引数は文の中のどこに置いてもよい)。引数の名前は、訳す人に意味がわかる名前にする (`{a}` ではなく `{device}`)。
- **ラベル (欄や項目の名前) を文の中に流用しない。** 文の中で欄を指すなら、その文を丸ごと別のキーにする。例: 数値の入力エラーは、欄ごとに `channel.invalid.port` などを持つ。
- **見た目が同じ日本語でも、役割が違えば別のキーにする。** 例: ボタンの「停止」(`main.tile.stop`、操作) と状態の「停止」(`status.stopped`)。ほかの言語では別の語になる。
- **単位と技術用語は英語のまま文に書いてよい。** ms、Mbps、GB、SRT、RTMP、latency、RTT、NVENC、x264 のプリセット名、Cam 1 など。言語ファイルの外 (C# や XAML) に直接書いてよいのは、こうした言語によらない表記だけ。
- **数による語形の変化 (複数形) は扱わない。** 数は「{count} 回」のように数字と単位で書く。
- **数値の書式 (桁数、小数) は C# で整えて文字列で渡す。** 今は Windows の地域設定に従う。
- **文言の入る部品の幅を固定しない。** 文の長さは言語で変わる (英語は日本語より長いことが多い)。
  - 設定画面のラベルの列は `Width="Auto" SharedSizeGroup="Label"` にする。画面 (タブ) の外側に `Grid.IsSharedSizeScope="True"` があるので、同じ画面の表すべてで、いちばん長いラベルの幅にそろう。表を足したら、ラベルの列にこの 2 つを付ける (付け忘れると、その表だけ列がずれる)。GroupBox の外の表は、中の表と列の位置を合わせるため左右に 6 あける (`Margin="6,0"`)。
  - 文言の入るプルダウンは `Width` ではなく `MinWidth` にする (長い項目では伸びる)。数値だけの入力欄や、言語によらない項目だけのプルダウンは固定幅でよい。
  - 直したら `./tools/ui_screenshots/run.ps1 -Language en` で英語の画面も撮り、見切れていないかを見る。
- **C++ (worker / engine) は文を作らない。** 画面に出すものは `ui_message("ID", {{"引数", 値}})` で ID と引数だけを送る。引数には値 (ホスト名、ポート、FFmpeg や SRT のエラー文字列など) を入れ、文を入れない。外部ライブラリのエラー文字列は英語のまま引数で渡す。

## 画面の文言を直すとき (VS Code)

engine や配信がなくても、受信中の状態の画面を出して、文言を直しながら確かめられる。

1. VS Code でこのフォルダを開く (おすすめの拡張機能は `.vscode/extensions.json`)。
2. 「ターミナル」→「タスクの実行」→「画面: 文言を確認 (メイン画面)」を選ぶ (流し込む状態は normal / degraded / ffmpeg-missing / disconnected / unreachable から選ぶ)。チャンネル設定・録画タブ・全体設定のタスクもある。
3. `src/ui/Denpachan.Ui/Lang/ja.json` や XAML・C# を直して保存すると、数秒でビルドし直して画面が起動し直す (`dotnet watch`)。止めるときはターミナルで Ctrl+C。

コマンドで動かすときは次のとおり (`--settings 1` / `--settings 1 --settings-tab 1` / `--general` で設定画面も開く)。

```powershell
dotnet watch --no-hot-reload run --project src/ui/Denpachan.Ui/Denpachan.Ui.csproj -- --fixture tools/ui_screenshots/cases/normal.json
```

- 画面に流し込む状態は `tools/ui_screenshots/cases/*.json` (スクリーンショット試験と同じもの)。
- XAML はホットリロードが効かないので、ホットリロードを切って毎回起動し直している。
- C++ のビルドは `build.ps1` (タスク「ビルド (全体)」) で行う。CMake Tools が開いたときに勝手に configure しないよう、`.vscode/settings.json` で止めている (同じ `out/build` を触って `build.ps1` とぶつかるため)。

## ファイルの形式

どの環境で保存しても同じ形になるよう、決まりをファイルに書いてある。VS Code では保存したときにこの形になる (`.vscode/settings.json`。おすすめの拡張機能を入れておく)。

| もの | 決まり | 書いてある場所 |
|------|--------|---------------|
| すべてのテキスト | UTF-8、改行は LF、末尾に改行 1 つ、行末に空白を残さない、字下げは空白 (4 桁。JSON・YAML・csproj・SVG は 2 桁) | `.editorconfig`、`.vscode/settings.json` |
| 改行コード | 取り出したときも LF (`core.autocrlf` の設定に左右されない) | `.gitattributes` |
| PowerShell (`.ps1`) | BOM 付き UTF-8 (Windows PowerShell 5.1 が日本語を読めるように) | `.editorconfig`、`.vscode/settings.json` |
| C++ | 保存時に clang-format で整える (1 行 140 桁まで) | `.clang-format` |
| C# | 保存時に C# 拡張機能で整える | `.editorconfig` の `[*.cs]` |
| JSON・YAML・XAML・Markdown・シェル・CMake | 書式の道具で全体を書き換えない (手で並べた形を保つ)。上の空白・改行の決まりだけ | `.vscode/settings.json` |

- 指摘だけする道具: PowerShell (PSScriptAnalyzer)、シェル (ShellCheck)、Markdown (markdownlint、`.markdownlint.jsonc`)。
- まとめて確かめる・整える: `./tools/format/check.ps1` (確かめるだけ) / `./tools/format/check.ps1 -Fix` (整える)。CI でも確かめ、合わなければ失敗する。
- clang-format は VS Code の C++ 拡張機能に付属のもの (版 23) を使う。Visual Studio に付属のもの (版 22) でも同じ結果になることを確かめてある。CI は pip の `clang-format==23.1.0`。版を上げて結果が変わったら、`-Fix` で整えてコミットする。

## アイコン

元は `assets/denpachan.svg`。これを直したら次を実行して `assets/denpachan.ico` を作り直し、両方をコミットする。

```powershell
./tools/icon/make_icon.ps1
```

- SVG を Edge (ヘッドレス) で描いて、16〜256 px を 1 つの ico にまとめる。ビルドには ico だけを使う (Edge は要らない)。
- 縮めた各サイズの PNG は `out/icon/` に残るので、小さいサイズで潰れていないかをそこで見る。
- 画面 (`denpachan.exe`) は csproj の `ApplicationIcon`、engine / worker / rtmpd / ctl は `src/common/icon.rc.in` で ico を埋め込む。ウィンドウとタスクバーも exe のアイコンを使う。
