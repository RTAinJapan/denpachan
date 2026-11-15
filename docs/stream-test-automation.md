# 配信テストの bot 向けのメトリクスの受信口 (WebSocket)

イベント前に各配信者と行う配信テストは、Discord bot ([RTAinJapan/techcheck-bot](https://github.com/RTAinJapan/techcheck-bot)) が進める。
配信者 → MediaConnect → 運営の経路のうち、denpachan は MediaConnect のフローの出力 (RTMP のときは ECS の RTMP サーバ) を受けて、
デコードの良否・音声のレベルを見て、録画を残す。bot がそれを読み、テスト用のチャンネルを操作できるように、
engine に **メトリクスの受信口** (WebSocket、待ち受けポート 1 つ) がある。この文書は、その受信口の仕様を書く。

- **bot の設計** (受付と待ちの列、テストの流れ、latency の調整、音声とその他のチェック、シートへの記録、bot の設定) は、
  [techcheck-bot の `docs/design.md`](https://github.com/RTAinJapan/techcheck-bot/blob/main/docs/design.md) にある。
  もとはこの文書に一緒に書いてあったものを、bot のリポジトリへ移した。
- **受信口の仕様は、この文書が正。** bot の設計書の §3.2 にも同じ内容があるが、変えるときはこの文書を直し、bot の設計書を合わせる。
- **節の番号は、bot の設計書と同じ** (1 つの文書だったときのまま)。この文書にあるのは §3.2 と、§9・§10 のうち engine に関わる部分だけ。
  「§5.6」のように、この文書に無い節の参照は、bot の設計書の節を指す。

目次:

- [3.2 denpachan のメトリクスの受信口](#32-denpachan-のメトリクスの受信口-mediaconnect--運営の区間デコード音声録画)
- [9. 実装](#9-実装)
- [10. 注意](#10-注意)

## 3.2 denpachan のメトリクスの受信口 (MediaConnect → 運営の区間、デコード、音声、録画)

今の IPC (`\\.\pipe\denpachan`、[design.md](design.md) §10.2) は名前付きパイプで、同じ PC の同じユーザーからしか繋げない。bot のために、**同じ内容を TCP の WebSocket で出す口**を engine に足す。画面と `denpachan-ctl` は今までどおりパイプを使う。

bot がここから使うのは: 受信の状態 (繋がっている、映像が出ている、途絶)、デコードの良否 (デコードエラー、fps、解像度)、音声のレベルと無音、録画の状態とファイル名。SRT の数字 (`latency` の項目) は MediaConnect → denpachan の区間のもので、**配信者の latency の判断には使わない** (この区間が悪いときは運営側の問題として別に伝える)。

#### 設定

`general.metrics` を足す (全体設定の画面にも出す)。

```json
"metrics": { "enabled": false, "bind": "127.0.0.1", "port": 9100, "tokenProtected": "", "allowControl": false }
```

| 項目 | 内容 |
| --- | --- |
| `enabled` | 既定 off。テストをイベント中も受け付けるので、テストの期間中 (イベント中を含む) は on |
| `bind` / `port` | 既定は loopback の 9100 (SRT の 9001〜9008 と離す)。RTMP サーバ (`rtmpServer.port`) と同じポートは設定を受け付けない。別の PC の bot から繋ぐなら `0.0.0.0` |
| `token` | 共有の秘密 (16〜128 文字、空白と制御文字なし)。**loopback 以外に bind するときは必須** (無ければ設定を受け付けない。設定ファイルを直接書き換えて無い状態になっていたら、engine は待ち受けずにログに出す)。パスフレーズと同じく DPAPI で保存する (`tokenProtected`)。IPC では返さず、設定済みかどうか (`hasToken`) だけ返す。画面に「作り直す」ボタン (32 バイトの乱数、Base64url。保存するまでの間だけ表示) |
| `allowControl` | 操作のメソッド (下) を許すか。既定 off (読むだけ)。bot にチャンネルと録画の開始・停止をさせるなら on。on でも、操作できるのは「配信テスト用」のチャンネルだけ。変えても受信口は開き直さない (要求ごとに見る) |

設定が変わると engine は受信口を開き直す。ポートが使われているなどで待ち受けられなければ、10 秒ごとにやり直す。

チャンネルの設定に **`testSlot`** (チャンネル設定の画面の「配信テスト用 (bot が操作してよい)」) を足す。運営がテストの枠を設定し直すとき (bot の設計書 §5.6) に、そのチャンネルに印を付け、外したチャンネルの印を外す。イベント中は同じ denpachan で本番のチャンネルが動いているので、bot の不具合や枠の設定の間違いで本番のチャンネルを止めたり録画を切ったりしないよう、**engine の側で**操作の対象を印のあるチャンネルに限る。

TLS はしない (家庭内 LAN か同じ PC で使う。外から繋ぐなら VPN か ssh のトンネルを通す)。

#### プロトコル

WebSocket (RFC 6455)、テキストフレーム、1 フレームに JSON 1 つ。

- 接続: `GET /ws HTTP/1.1` + `Upgrade: websocket`。ほかのパスは `404`。
- 認証: トークンを設定していれば、`Authorization: Bearer <token>` ヘッダか `?token=<token>` (ヘッダを付けられないクライアント向け。ほかの引数と並んでいてもよい)。合わなければ `401`。トークンが空 (loopback のとき) なら認証しない。
- **ブラウザからの接続は断る**: `Origin` ヘッダが付いていれば `403`。loopback でトークン無しのとき、利用者が開いた Web ページのスクリプトから繋がれないようにするため (bot の Node.js の WebSocket は `Origin` を付けない)。
- 繋がったら engine が `hello` を 1 つ送る。以後 1 秒ごとに `channels` を送る (engine の 200 ms の監視の 5 回に 1 回、`state` の通知と同時に作る)。
- 要求 / 応答はパイプと同じ形: `{"id": 1, "method": "getState", "params": {}}` → `{"id": 1, "result": ...}` / `{"id": 1, "error": "..."}`。壊れた JSON には `id` が null の `error` を返し、切らない。受け取る 1 メッセージは 1 MB まで。
- ping / pong は WebSocket の制御フレームで。engine は 10 秒ごとに ping を送り、30 秒何も届かない相手を切る。クライアントからの close には、受け取ったコードをそのまま入れた close を返してから切る。
- 同時接続は 8 まで (9 本目は `503`)。書き込みが 1 秒待っても進まない相手 (受け取らない bot) は切る。**engine の tick を止めない** (パイプの `broadcast` と同じ作り: クライアントごとのスレッドとキュー (64 件)、溢れたら古いものから捨てる)。

#### `channels` メッセージ

1 秒ごと。全チャンネル分 (8 チャンネル。止まっているチャンネルも `status: "stopped"` で入れる。bot が「どのチャンネルが空いているか」を知るため)。項目の組み立ては `src/engine/metrics_view.cpp`。

```json
{
  "event": "channels",
  "time": "2026-11-01T12:34:56.789Z",
  "channels": [
    {
      "ch": 1, "name": "Runner A", "enabled": true, "inUse": true, "testSlot": true,
      "status": "live",
      "source": { "protocol": "srt", "mode": "caller", "host": "203.0.113.5", "port": 5000, "peer": "203.0.113.5:5000", "connectedSec": 83 },
      "video": { "codec": "h264", "width": 1920, "height": 1080, "fps": 59.9, "outputFps": 60, "bitrateMbps": 6.1, "hwDecode": true,
                 "decodeErrors": 0, "droppedLate": 0, "droppedQueue": 0 },
      "latency": {
        "effectiveMs": 300, "receiverMs": 300,
        "rttMs": 12.0, "lossPct": 0.0, "drops": 0, "windowSec": 60,
        "recommendMs": 40, "multiplier": 3,
        "headroomMs": 260, "droppedLastMin": 0,
        "verdict": "generous"
      },
      "tcp": null,
      "audio": { "peakDb": [-9.5, -9.8], "gaps": 0, "silentSec": 0 },
      "record": { "state": "recording", "file": "D:\\rec\\1 - 202611\\01 12-33-10.mkv", "seconds": 106 },
      "rejectedSenders": 0,
      "lastError": null
    }
  ]
}
```

| 項目 | 元 (`state` の項目) | 意味 |
| --- | --- | --- |
| `inUse` | 全体設定のチャンネル数 | このチャンネルが全体設定のチャンネル数の内か |
| `status` | `worker.state`、`video.state`、`video.connected`、`lastError` | 画面のタイルとほぼ同じ区分: `stopped` / `starting` (worker は動いているが、まだメトリクスが届かない。画面の「起動中」) / `listening` / `connecting` (caller・pull) / `waiting` (繋がったが映像がまだ) / `live` / `hold` (途絶、最後の絵を保持) / `unreachable` (caller・pull で繋がらない。`lastError` に理由) / `restarting` / `unavailable` (FFmpeg が無い)。画面と違うのは、listener (SRT listener、RTMP push) が繋がって映像がまだのときも `waiting` にすること (画面は「待ち受け中」のまま。bot は繋がったかを知りたいので細かく分ける) |
| `source` | チャンネルの設定 | `protocol`、`mode`。SRT は `port` (caller なら `host` も)、RTMP push は `app`。`peer` は繋がっている相手 |
| `source.connectedSec` | `video.connected` が立ってからの秒数 (engine で数える) | 繋がってからの時間。繋ぎ直すと 0 に戻る |
| `video` | `video.*` | worker のメトリクスが無いとき (止まっている、起動中) は null。`fps` は `video.decodeFps` (受信した映像の fps) |
| `latency` | `srt.latencyMs`、`srt.configuredLatencyMs`、`srt.recommend.*` | SRT のときだけ (それ以外は null)。**MediaConnect → denpachan の区間**の、実際の latency、受信側の設定値、RTT・ロス率・取りこぼし・窓の秒数、推奨値と倍率、余裕 (`effectiveMs − recommendMs`。計測中は null)、直近 60 秒の取りこぼし (`droppedLastMin`)、判定 |
| `latency.verdict` | 画面の経路の行の色と同じ規則 | `measuring` (推奨値がまだ無い) / `insufficient` (直近 60 秒に取りこぼしがある = 画面の赤) / `tight` (推奨値 > 実際の latency = 画面の黄) / `ok` / `generous` (実際の latency が推奨値の 2 倍以上) |
| `tcp` | `tcp.*` | RTMP push のときだけ (それ以外は null。**RTMP pull には TCP の統計が無い**)。`rttMs`、`retransBytes`、直近 60 秒の再送 (`retransBytesLastMin`)、`verdict` (`ok` / 再送があれば `tight`) |
| `audio` | `audioIn.*` | worker のメトリクスが無いときは null。`peakDb` (左右、dBFS)、`gaps`、`silentSec` (両チャンネルとも −60 dBFS 未満が続いた秒数。engine で数える) |
| `record` | `record.*` | worker のメトリクスが無いときは null。`state`、`file`、`seconds` (録画の開始からの秒数。bot はチェックの手順の録画の区間に使う) |
| `rejectedSenders` | `video.rejectedSenders` | 同じ宛先に別の送出元がつないできて断った数 |
| `lastError` | `video.lastError` | worker のメッセージ (`{"id": "...", ...}` の形の JSON) か null |

bot は denpachan の区間の数字を判定には使わず、記録 (運営の区間の RTT・取りこぼし) と、運営側の問題の切り分けに使う。配信者の区間の判定は MediaConnect の数字 (bot の設計書 §3.1) で行う。

`hello`:

```json
{ "event": "hello", "version": "0.2.0", "channelCount": 4, "allowControl": true, "intervalMs": 1000 }
```

`channelCount` は全体設定のチャンネル数、`allowControl` は今の設定の値。

#### メソッド (要求 / 応答)

パイプと同じメソッドを、allowlist で絞って通す。ほかのメソッド (`patchGeneral`、`shutdown` など) は `error`。

| 種別 | メソッド | 条件 |
| --- | --- | --- |
| 読む | `getState`、`getConfig` (IPC 形。RTMP のキーと pull の URL は外して `hasKey` / `hasUrl` だけ、受信口のトークンは `hasToken` だけ)、`getChannels` (`channels` と同じ内容を 1 回) | いつでも |
| 操作 | `startChannel`、`stopChannel`、`setRecording` (`enabled` は bool)、`patchChannel` | `allowControl` が on で、**対象のチャンネルが `testSlot`** のとき (でなければ `error`) |

`patchChannel` は bot が壊せる範囲を狭める: **`source.latencyMs` と `name` だけ**を受け付ける (他の項目が入っていれば `error`)。応答のチャンネルの設定からも秘密は外す。MediaConnect 経由では bot が denpachan の latency を変える必要はほぼ無いが、MediaConnect → denpachan の区間が悪いときに運営が `/techcheck latency-receiver` で上げられるよう残す。SRT の枠と RTMP の枠は別のチャンネルにしておき、bot はプロトコルの切り替えでチャンネルの設定を変えない。

#### 実装 (engine)

- `src/engine/ws_server.cpp/h`: TCP の待ち受け、HTTP の Upgrade、フレームの読み書き (テキスト、ping/pong、close。クライアントからのマスクを外す)、クライアントごとのスレッドと送信のキュー。`IpcServer` と同じ `Handler` と `broadcast` の形。
  - 握手の `Sec-WebSocket-Accept` は SHA-1 + Base64。SHA-1 は Windows の `BCryptHash` (新しい依存なし)。
  - 外部のライブラリは入れない (使うのはテキストフレームの送受信だけで、rtmpd と同じく自前で十分)。
- `src/engine/metrics_view.cpp/h`: engine の `state` から `channels` を組み立てる (`ChannelsView`)。`connectedSec`、`silentSec`、直近 60 秒の取りこぼし・再送はここでチャンネルごとに数える。
- `main.cpp` の 200 ms ごとの監視の 5 回に 1 回、`Engine::state_with_channels()` で `state` (パイプへ) と `channels` (受信口へ) を作って流す。受信口の設定が変わったら開き直す。
- `Engine::handle_remote` で allowlist、`testSlot` の確認、`patchChannel` の絞り込みをしてから、パイプと同じ処理に回す。`testSlot` はチャンネルの設定 (`config.json`) にあり、画面のチャンネル設定に印を出す (タイルにも「テスト用」と出す)。
- [design.md](design.md) §10.2 に「WebSocket の受信口」の項がある。試験は `tools/testsrc/ws_test.sh` (Python の websockets を使う)。

## 9. 実装

| 部分 | 場所 | 状況 |
| --- | --- | --- |
| engine: `general.metrics` の設定、WebSocket の受信口、`hello` / `channels`、要求の絞り (`allowControl`、`testSlot`、`patchChannel`)、画面 (全体設定、チャンネル設定の印、タイルの「テスト用」) | denpachan (#31) | 済み。試験は `tools/testsrc/ws_test.sh` (#34) |
| bot | [RTAinJapan/techcheck-bot](https://github.com/RTAinJapan/techcheck-bot) | bot の設計書 §9 |

## 10. 注意

engine に関わるものだけ。bot の側の注意は、bot の設計書 §10。

- **イベント中のテスト**: 本番のチャンネルと同じ denpachan で受けるので、本番を触らない仕組みを二重にする (bot は設定された枠のチャンネルしか操作しない。engine は `testSlot` の印の無いチャンネルへの操作を断る)。テストのチャンネルのデコードの分だけ PC の負荷が増えるので、状態バーの CPU・GPU を見ておく。
- **bot が落ちたとき**: engine は影響を受けない。
- **engine の受信口をイベント中も開けておく**: テストをイベント中も受け付けるので、受信口はイベント中も on になる。`bind: 127.0.0.1` のまま bot を同じ PC で動かすのが安全 (外から繋がらない)。別の PC で動かすならトークンを必ず付ける。受信口と bot が落ちても、本番の受信には影響しない (受信口は engine の tick を止めない作り。§3.2)。
- **実物で確かめていないこと**: bot と denpachan の受信口を実際に繋いだ動作 (bot の側は、`channels` の形を `src/engine/metrics_view.cpp` に合わせて試験している)。最初に本物の AWS のアカウントと denpachan で一度通す。
