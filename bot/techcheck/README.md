# techcheck bot (配信テストの Discord bot)

イベント前の配信テストを Discord で進める bot。設計は [docs/stream-test-automation.md](../../docs/stream-test-automation.md)。

> このフォルダは、あとで別のリポジトリに移す。フォルダごと移せば動くように、denpachan の本体のコードには依存しない (denpachan とはメトリクスの受信口の WebSocket だけでやりとりする)。

## できること

- 配信者が配信テストのチャンネルで `/techcheck start` を実行すると、枠 (SRT 1 つ) が空いていればプライベートスレッドを作って案内する。空いていなければ待ちの列に入れ、順番が来たらメンションと DM で知らせる。
- 配信の開始を denpachan と CloudWatch で検出し、2 分ほど配信させて映像を判定する。乱れていれば latency の値を決めて OBS を直してもらい、直らなければ ECS の RTMP サーバに案内する。
- 音声とその他のチェック (ゲーム音、静か、マイク、PC の音、画面、繋ぎ直し) を順にさせ、録画の区間と参考の判定を記録する。
- 終わったらシートに 1 行書き、Discord webhook で結果を通知し (URL が未設定なら送らない)、スレッドをアーカイブする。
- 配信者向けの文言は日本語と英語 (`messages/ja.yaml`、`messages/en.yaml`)。

## 動かす

Node.js 22.13 以上 (SQLite は Node 付属の `node:sqlite` を使う)。

```sh
npm ci
cp config.example.yaml config.yaml   # 書き換える
npm run build
npm start                            # または node dist/src/index.js path/to/config.yaml
```

- 起動すると、設定したサーバに `/techcheck` を登録する (サーバのコマンドなので、すぐ反映される)。
- bot に要る Discord の権限: 配信テストのチャンネルでメッセージの送信、プライベートスレッドの作成、スレッドでのメッセージの送信、スレッドの管理 (ロックとアーカイブ)。運営への通知のチャンネルでメッセージの送信。
- AWS の IAM ユーザーに要る権限: `mediaconnect:DescribeFlow`、`mediaconnect:StartFlow`、`mediaconnect:StopFlow`、`mediaconnect:DescribeFlowSourceMetadata`、`mediaconnect:DescribeFlowSourceThumbnail`、`cloudwatch:GetMetricData`。
- シートは、サービスアカウントのメールアドレスに編集の権限を共有しておく。1 行目が空なら見出しを書く。
- denpachan の側では、全体設定でメトリクスの受信口を有効にし (`allowControl` も on)、枠に使うチャンネルに「配信テスト用」の印を付ける。

## 試験

```sh
npm test          # 外 (Discord、AWS、denpachan、シート) を偽物にして、テストの流れを 1 秒ずつ進めて確かめる
npm run typecheck
```

## 実物で確かめていないこと

- CloudWatch のメトリクスのディメンション名 (`FlowARN`) と 1 秒の粒度で取れること。AWS のドキュメントの記述どおりにしてあるが、実際のアカウントで一度確かめる (`src/adapters/aws.ts`)。
- `DescribeFlowSourceMetadata` の `StreamType` と `FrameRate` の実際の値の形。
- denpachan のメトリクスの受信口は別の PR で作っている。`channels` メッセージの形は設計書 §3.2 に合わせてある。

## 構成

| ファイル | 内容 |
| --- | --- |
| `src/service.ts` | テストの進行 (受付、待ちの列、枠、案内、映像の判定、latency の調整、RTMP、チェック、終わり)。1 秒ごとの `tick()` で進める |
| `src/advisor.ts` | latency の推奨値と判定 (denpachan の `srt_advisor.h` と同じ規則) |
| `src/checks.ts` | チェックの既定の手順と、音声のレベルによる参考の判定 |
| `src/record.ts` | シートの 1 行と webhook の本文 |
| `src/config.ts` | 設定ファイルの読み込みと検証 |
| `src/i18n.ts` | 日本語 / 英語の文言 |
| `src/ports.ts` | 外とのやりとりの口 (本物は `src/adapters/` と `src/discord/`、試験では `test/helpers.ts` の偽物) |
| `src/adapters/` | MediaConnect / CloudWatch、denpachan の受信口、シート、webhook、DNS |
| `src/discord/` | `/techcheck` の定義と、コマンド・ボタン・モーダル・スレッド |
