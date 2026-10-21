#!/usr/bin/env bash
# 0.1.x (4ch) の設定からの引き継ぎの試験。
#
# ch1 が 9005 番 (8ch 化で増えた ch5 の既定のポート) で待ち受ける 0.1.x の設定で engine を起動し、次を確かめる。
# - 設定が捨てられない (.broken に退避されない)
# - 使うチャンネル数を 5 に増やせる。ch5 のポートはぶつからない番号に変わる
# - 使っているチャンネル同士の重複は、これまでどおり拒否される
#
# 試験用の設定ファイル (out/check/upgrade-config.json) を使うので、普段の設定は触らない。
#
#   tools/testsrc/config_upgrade_test.sh
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$ROOT/out/build/bin/Release"
OUT="$ROOT/out/check"
CFG="$OUT/upgrade-config.json"
LOG="$OUT/upgrade-engine.log"
CTL="$BIN/denpachan-ctl.exe"
mkdir -p "$OUT"
taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
sleep 1
rm -f "$CFG" "$CFG.broken" "$LOG"
FAIL=0
check() {  # check <説明> <条件の結果 (0 = OK)>
    if [ "$2" = 0 ]; then echo "OK   $1"; else echo "FAIL $1"; FAIL=1; fi
}
port_of() { "$CTL" set "$1" '{}' 2>&1 | grep -oE '"port": [0-9]+' | grep -oE '[0-9]+'; }

# 0.1.x の設定: channelCount が無く、チャンネルは 4 つだけ
cat >"$CFG" <<'EOF'
{
  "version": 1,
  "general": { "language": "auto", "stallMs": 2000, "audioMarginMs": 10, "avSync": true },
  "channels": [
    { "id": 1, "name": "A", "enabled": false, "source": { "protocol": "srt", "mode": "listener", "host": "0.0.0.0", "port": 9005 }, "vcam": 1 },
    { "id": 2, "name": "B", "enabled": false, "source": { "protocol": "srt", "mode": "listener", "host": "0.0.0.0", "port": 9002 }, "vcam": 2 },
    { "id": 3, "name": "C", "enabled": false, "source": { "protocol": "srt", "mode": "listener", "host": "0.0.0.0", "port": 9003 }, "vcam": 3 },
    { "id": 4, "name": "D", "enabled": false, "source": { "protocol": "srt", "mode": "listener", "host": "0.0.0.0", "port": 9004 }, "vcam": 4 }
  ]
}
EOF

"$BIN/denpachan-engine.exe" --config "$CFG" --log-file "$LOG" >/dev/null 2>&1 &
sleep 2

[ ! -f "$CFG.broken" ] && ! grep -q "config is invalid" "$LOG"
check "1. 0.1.x の設定がそのまま読める (.broken に退避されない)" $?
[ "$(port_of 1)" = 9005 ]
check "2. ch1 のポートは 9005 のまま (got $(port_of 1))" $?

"$CTL" set-general '{"channelCount":5}' >/dev/null 2>&1
check "3. 使うチャンネル数を 5 に増やせる" $?
P5=$(port_of 5)
[ -n "$P5" ] && [ "$P5" != 9005 ] && [ "$P5" != 9002 ] && [ "$P5" != 9003 ] && [ "$P5" != 9004 ]
check "4. ch5 のポートはぶつからない番号に変わる (got $P5)" $?

! "$CTL" set 5 '{"source":{"port":9005}}' >/dev/null 2>&1
check "5. 使っているチャンネル同士の重複は拒否される" $?

"$CTL" set-general '{"channelCount":4}' >/dev/null 2>&1
check "6. 4 に戻せる" $?

taskkill //IM denpachan-engine.exe //F >/dev/null 2>&1
exit $FAIL
