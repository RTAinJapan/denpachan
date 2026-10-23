#!/usr/bin/env bash
# メトリクスの受信口 (WebSocket、配信テストの bot 向け) の試験 (Python の websockets を使う)。
#
# 0. 設定の検証: loopback 以外でトークン無し / RTMP サーバと同じポート / 短いトークンは断る。トークンは暗号化して保存する
# 1. basic: hello と channels (1 秒ごと)、読むメソッド (getConfig は秘密を外す)、操作の拒否 (allowControl off)、
#    壊れた JSON、ping / pong、close の応答 (1000)
# 2. control: allowControl on でも、配信テスト用の印 (testSlot) の無いチャンネルと、name / source.latencyMs 以外の変更は断る
# 3. limits: /ws 以外は 404、ブラウザ (Origin 付き) は 403、9 本目は 503
# 4. token: トークン無し / 違うトークンは 401、Bearer と ?token= で繋がる
# 5. idle: ping は 10 秒ごと、pong を返さない相手は 30 秒で切る
# 6. 受信口を閉じたら待ち受けない
# 試験用の設定ファイル (out/check/ws-config.json) を使う。
#
#   tools/testsrc/ws_test.sh
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
. "$ROOT/tools/testsrc/dev_env.sh"
BIN="$ROOT/out/build/bin/Release"
OUT="$ROOT/out/check"
CTL="$BIN/denpachan-ctl.exe"
CFG="$OUT/ws-config.json"
LOG="$OUT/ws-engine.log"
PY="$ROOT/tools/testsrc/ws_test.py"
PORT=9100
TOKEN="0123456789abcdefghijklmnopqrstuvwxyzABCDEFG"
mkdir -p "$OUT"
if ! python -I -c "import websockets" 2>/dev/null; then
    echo "SKIP: Python の websockets がありません (pip install websockets)"
    exit 0
fi
FAIL=0
check() {  # check <説明> <0 = OK>
    if [ "$2" = 0 ]; then echo "OK   $1"; else echo "FAIL $1"; FAIL=1; fi
}
phase() {  # phase <名前> [引数...]: ws_test.py を実行し、失敗があれば FAIL にする
    python -I "$PY" "$PORT" "$@" || FAIL=1
}

taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1
sleep 1
rm -f "$CFG" "$LOG"
"$BIN/denpachan-engine.exe" --config "$CFG" --log-file "$LOG" >/dev/null 2>&1 &
sleep 2

echo "==== 0. 設定の検証"
R=$("$CTL" set-general '{"metrics":{"enabled":true,"bind":"0.0.0.0","port":9100}}' 2>&1)
grep -q "token is required" <<<"$R"
check "0a. loopback 以外でトークン無しは断る ($R)" $?
R=$("$CTL" set-general '{"metrics":{"enabled":true,"bind":"127.0.0.1","port":1935}}' 2>&1)
grep -q "must differ from rtmpServer.port" <<<"$R"
check "0b. RTMP サーバと同じポートは断る ($R)" $?
R=$("$CTL" set-general '{"metrics":{"enabled":true,"bind":"127.0.0.1","port":9100,"token":"short"}}' 2>&1)
grep -q "16-128 characters" <<<"$R"
check "0c. 16 文字未満のトークンは断る ($R)" $?

"$CTL" set-general "{\"metrics\":{\"enabled\":true,\"bind\":\"127.0.0.1\",\"port\":$PORT,\"allowControl\":false}}" >/dev/null
"$CTL" set 1 '{"testSlot":true,"source":{"protocol":"srt","mode":"listener","port":9001}}' >/dev/null
sleep 1.5
echo "==== 1. basic (トークン無し、allowControl off)"
phase basic
echo "==== 2. control (allowControl on)"
"$CTL" set-general '{"metrics":{"allowControl":true}}' >/dev/null
sleep 1
phase control
echo "==== 3. limits"
phase limits
echo "==== 4. token"
"$CTL" set-general "{\"metrics\":{\"token\":\"$TOKEN\"}}" >/dev/null
sleep 1.5
phase token "$TOKEN"
grep -q '"tokenProtected": "AQAAA' "$CFG" && ! grep -q '"token":' "$CFG"
check "4b. 設定ファイルには暗号化したトークン (tokenProtected) だけを書く" $?
echo "==== 5. idle (約 30 秒)"
"$CTL" set-general '{"metrics":{"token":""}}' >/dev/null
sleep 1.5
phase idle
echo "==== 6. 受信口を閉じる"
"$CTL" set-general '{"metrics":{"enabled":false}}' >/dev/null
sleep 1.5
phase closed

"$CTL" stop 1 >/dev/null 2>&1
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
taskkill //IM denpachan-worker.exe //F >/dev/null 2>&1
echo "---- engine log"
grep -E "metrics: .*(refused|listening|stopped|did not answer)" "$LOG" | cut -c12- | head -20
exit $FAIL
