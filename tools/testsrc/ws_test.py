"""メトリクスの受信口 (WebSocket) を叩くクライアント (tools/testsrc/ws_test.sh から呼ぶ。Python の websockets を使う)。

    python -E tools/testsrc/ws_test.py <port> basic            hello / channels、読むメソッド、操作の拒否、ping、close
    python -E tools/testsrc/ws_test.py <port> control          allowControl on のときの操作の制限 (ch1 に testSlot が要る)
    python -E tools/testsrc/ws_test.py <port> limits           404 / 403 (Origin) / 503 (9 本目)
    python -E tools/testsrc/ws_test.py <port> token <token>    401、Bearer と ?token=
    python -E tools/testsrc/ws_test.py <port> idle             ping の間隔、pong を返さない相手を切るまで (約 30 秒、最大 45 秒)
    python -E tools/testsrc/ws_test.py <port> closed           待ち受けていないこと

項目ごとに OK / FAIL を出し、FAIL があれば終了コード 1。
ほかの試験の .py と違い -I ではなく -E で動かす (-I は -s を含み、`pip install --user` で入れた websockets が見えない)。
"""

import asyncio
import base64
import json
import os
import socket
import struct
import sys
import time

from websockets.asyncio.client import connect
from websockets.exceptions import InvalidStatus

# bash から呼ぶと標準出力が cp932 になり、日本語の行が化ける (-E では PYTHONIOENCODING も効かない)
sys.stdout.reconfigure(encoding="utf-8")

PORT = int(sys.argv[1])
PHASE = sys.argv[2]
TOKEN = sys.argv[3] if len(sys.argv) > 3 else ""
URL = f"ws://127.0.0.1:{PORT}/ws"
FAIL = 0


def check(desc, ok, extra=""):
    global FAIL
    print(("OK   " if ok else "FAIL ") + desc + (f" ({extra})" if extra else ""))
    if not ok:
        FAIL = 1


async def recv_event(ws, name, timeout=5):
    deadline = time.monotonic() + timeout
    while True:
        msg = json.loads(await asyncio.wait_for(ws.recv(), max(0.1, deadline - time.monotonic())))
        if msg.get("event") == name:
            return msg


_next_id = 1


async def call(ws, method, params=None, timeout=5):
    global _next_id
    rid = _next_id
    _next_id += 1
    await ws.send(json.dumps({"id": rid, "method": method, "params": params or {}}))
    deadline = time.monotonic() + timeout
    while True:
        msg = json.loads(await asyncio.wait_for(ws.recv(), max(0.1, deadline - time.monotonic())))
        if "event" in msg:
            continue
        if msg.get("id") == rid:
            return msg


def status_of(exc):
    return getattr(getattr(exc, "response", None), "status_code", None)


async def basic():
    async with connect(URL, open_timeout=5) as ws:
        hello = json.loads(await asyncio.wait_for(ws.recv(), 5))
        check("1. 最初に hello が届く", hello.get("event") == "hello", json.dumps(hello))
        ev = await recv_event(ws, "channels")
        chs = ev.get("channels", [])
        check("2. channels が届く", len(chs) >= 1 and "time" in ev, f"{len(chs)} ch, time {ev.get('time')}")
        ch1 = next((c for c in chs if c.get("ch") == 1), {})
        check("3. ch1 に testSlot=true が出る", ch1.get("testSlot") is True, json.dumps(ch1)[:300])
        for key in ("status", "source", "video", "latency", "tcp", "audio", "record", "rejectedSenders", "lastError", "inUse"):
            check(f"3.{key} ch1 に {key} がある", key in ch1, repr(ch1.get(key))[:120])
        await recv_event(ws, "channels", 5)
        t1 = time.monotonic()
        await recv_event(ws, "channels", 5)
        interval = time.monotonic() - t1
        check("4. channels は約 1 秒ごと", 0.7 < interval < 1.4, f"{interval:.2f} s")

        r = await call(ws, "getState")
        check("5. getState が通る", "result" in r and "channels" in r["result"], str(r)[:120])
        r = await call(ws, "getChannels")
        check("6. getChannels が通る", r.get("result", {}).get("event") == "channels", str(r)[:120])
        r = await call(ws, "getConfig")
        cfg = r.get("result", {})
        src = cfg.get("channels", [{}])[0].get("source", {})
        check("7. getConfig の source に key / url が無い", "key" not in src and "url" not in src and "hasKey" in src and "hasUrl" in src,
              json.dumps(src))
        m = cfg.get("general", {}).get("metrics", {})
        check("8. getConfig の metrics に token が無い (hasToken だけ)", "token" not in m and "hasToken" in m, json.dumps(m))
        r = await call(ws, "startChannel", {"ch": 1})
        check("9. allowControl が off なら startChannel は断られる", "error" in r, str(r)[:160])
        r = await call(ws, "patchGeneral", {"patch": {}})
        check("10. patchGeneral は断られる", "error" in r, str(r)[:160])
        r = await call(ws, "shutdown", {})
        check("11. 知らないメソッドは断られる", "error" in r, str(r)[:160])
        await ws.send("{not json")
        deadline = time.monotonic() + 5
        r = None
        while time.monotonic() < deadline:
            msg = json.loads(await asyncio.wait_for(ws.recv(), 5))
            if "event" not in msg:
                r = msg
                break
        check("12. 壊れた JSON にはエラーの応答 (切らない)", r is not None and "error" in r and r.get("id") is None, str(r)[:160])
        pong = await ws.ping()
        await asyncio.wait_for(pong, 5)
        check("13. ping に pong が返る", True)
        await ws.close()
        check("14. こちらから閉じると close の応答が返る (1000)", ws.close_code == 1000, f"close_code {ws.close_code}")


async def control():
    async with connect(URL, open_timeout=5) as ws:
        hello = json.loads(await asyncio.wait_for(ws.recv(), 5))
        check("1. hello の allowControl が true", hello.get("allowControl") is True, json.dumps(hello))
        r = await call(ws, "startChannel", {"ch": 2})
        check("2. 印の無い ch2 の startChannel は断られる", "error" in r, str(r)[:160])
        r = await call(ws, "startChannel", {"ch": 9})
        check("3. ch 9 は断られる", "error" in r, str(r)[:160])
        r = await call(ws, "patchChannel", {"ch": 1, "patch": {"source": {"port": 9999}}})
        check("4. patchChannel で port は変えられない", "error" in r, str(r)[:160])
        r = await call(ws, "patchChannel", {"ch": 1, "patch": {"record": {"enabled": True}}})
        check("5. patchChannel で record は変えられない", "error" in r, str(r)[:160])
        r = await call(ws, "patchChannel", {"ch": 1, "patch": {"source": {"latencyMs": 300}}})
        res = r.get("result", {})
        check("6. patchChannel で latencyMs は変えられる (応答に key は無い)",
              res.get("source", {}).get("latencyMs") == 300 and "key" not in res.get("source", {}), str(r)[:200])
        r = await call(ws, "patchChannel", {"ch": 1, "patch": {"name": "bot test"}})
        check("7. patchChannel で name は変えられる", r.get("result", {}).get("name") == "bot test", str(r)[:160])
        r = await call(ws, "startChannel", {"ch": 1})
        check("8. 印のある ch1 の startChannel は通る", "result" in r, str(r)[:160])
        ev = await recv_event(ws, "channels", 8)
        ch1 = next((c for c in ev["channels"] if c["ch"] == 1), {})
        for _ in range(6):
            if ch1.get("status") in ("listening", "live", "waiting"):
                break
            ev = await recv_event(ws, "channels", 8)
            ch1 = next((c for c in ev["channels"] if c["ch"] == 1), {})
        check("9. 開始後の ch1 の status", ch1.get("status") in ("listening", "live", "waiting", "starting"),
              f"status {ch1.get('status')} latency {json.dumps(ch1.get('latency'))}")
        r = await call(ws, "setRecording", {"ch": 1, "enabled": False})
        check("10. setRecording が通る", "result" in r, str(r)[:160])
        r = await call(ws, "setRecording", {"ch": 1, "enabled": "yes"})
        check("11. setRecording の enabled が bool でなければ断られる", "error" in r, str(r)[:160])
        r = await call(ws, "stopChannel", {"ch": 1})
        check("12. stopChannel が通る", "result" in r, str(r)[:160])


async def token():
    try:
        async with connect(URL, open_timeout=5) as ws:
            await ws.recv()
        check("1. トークン無しは断られる (401)", False, "connected")
    except InvalidStatus as e:
        check("1. トークン無しは断られる (401)", status_of(e) == 401, f"status {status_of(e)}")
    try:
        async with connect(URL, open_timeout=5, additional_headers={"Authorization": "Bearer " + TOKEN[:-1] + "x"}) as ws:
            await ws.recv()
        check("2. 違うトークンは断られる (401)", False, "connected")
    except InvalidStatus as e:
        check("2. 違うトークンは断られる (401)", status_of(e) == 401, f"status {status_of(e)}")
    async with connect(URL, open_timeout=5, additional_headers={"Authorization": "Bearer " + TOKEN}) as ws:
        hello = json.loads(await asyncio.wait_for(ws.recv(), 5))
        check("3. Authorization: Bearer で繋がる", hello.get("event") == "hello")
    async with connect(URL + "?token=" + TOKEN, open_timeout=5) as ws:
        hello = json.loads(await asyncio.wait_for(ws.recv(), 5))
        check("4. ?token= で繋がる", hello.get("event") == "hello")
    async with connect(URL + "?a=1&token=" + TOKEN + "&b=2", open_timeout=5) as ws:
        hello = json.loads(await asyncio.wait_for(ws.recv(), 5))
        check("5. ?token= がほかの引数の間にあっても繋がる", hello.get("event") == "hello")


async def limits():
    try:
        async with connect(f"ws://127.0.0.1:{PORT}/other", open_timeout=5) as ws:
            await ws.recv()
        check("1. /ws 以外は 404", False, "connected")
    except InvalidStatus as e:
        check("1. /ws 以外は 404", status_of(e) == 404, f"status {status_of(e)}")
    # ブラウザのページからの接続 (Origin 付き)。トークン無しの loopback で通ってしまうか
    try:
        async with connect(URL, open_timeout=5, origin="http://evil.example") as ws:
            hello = json.loads(await asyncio.wait_for(ws.recv(), 5))
        check("2. Origin 付き (ブラウザのページから) の接続は断られる", False, "connected: " + json.dumps(hello)[:80])
    except InvalidStatus as e:
        check("2. Origin 付き (ブラウザのページから) の接続は断られる (403)", status_of(e) == 403, f"status {status_of(e)}")
    conns = []
    try:
        for i in range(8):
            conns.append(await connect(URL, open_timeout=5))
        check("3. 8 本まで繋がる", True)
        try:
            extra = await connect(URL, open_timeout=5)
            await extra.recv()
            check("4. 9 本目は断られる (503)", False, "connected")
            await extra.close()
        except InvalidStatus as e:
            check("4. 9 本目は断られる (503)", status_of(e) == 503, f"status {status_of(e)}")
    finally:
        for c in conns:
            await c.close()
    await asyncio.sleep(1.5)
    async with connect(URL, open_timeout=5) as ws:
        hello = json.loads(await asyncio.wait_for(ws.recv(), 5))
        check("5. 8 本切ったあとはまた繋がる", hello.get("event") == "hello")


def idle():
    # 生のソケットで握手だけして、ping に答えない。30 秒で切られるか、ping は 10 秒ごとか
    s = socket.create_connection(("127.0.0.1", PORT), timeout=60)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET /ws HTTP/1.1\r\nHost: 127.0.0.1:{PORT}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += s.recv(4096)
    head, buf = buf.split(b"\r\n\r\n", 1)
    check("1. 握手が 101 で返る", head.startswith(b"HTTP/1.1 101"), head.split(b"\r\n")[0].decode())
    t0 = time.monotonic()
    pings = []
    texts = 0
    closed_at = None
    # 期限を付ける: engine は channels を 1 秒ごとに送るので recv は毎秒返り、ソケットのタイムアウトだけでは、
    # 受信口が切らなくなったときに試験が終わらない。期限に達したら closed_at = None のまま抜けて FAIL にする
    s.settimeout(5)
    deadline = t0 + 45
    while time.monotonic() < deadline:
        try:
            while True:
                if len(buf) >= 2:
                    op = buf[0] & 0x0F
                    ln = buf[1] & 0x7F
                    hdr = 2
                    if ln == 126:
                        if len(buf) < 4:
                            break
                        ln = struct.unpack("!H", buf[2:4])[0]
                        hdr = 4
                    elif ln == 127:
                        if len(buf) < 10:
                            break
                        ln = struct.unpack("!Q", buf[2:10])[0]
                        hdr = 10
                    if len(buf) < hdr + ln:
                        break
                    if op == 0x9:
                        pings.append(time.monotonic() - t0)
                    elif op == 0x1:
                        texts += 1
                    buf = buf[hdr + ln:]
                else:
                    break
            d = s.recv(65536)
            if not d:
                closed_at = time.monotonic() - t0
                break
            buf += d
        except TimeoutError:
            continue  # 何も届かないだけ (切られてはいない)
        except OSError:  # ConnectionResetError / ConnectionAbortedError など
            closed_at = time.monotonic() - t0
            break
    s.close()
    check("2. ping は約 10 秒ごと", len(pings) >= 2 and all(8 < b - a < 13 for a, b in zip(pings, pings[1:])),
          "at " + ", ".join(f"{p:.1f}" for p in pings))
    check("3. pong を返さないと約 30 秒で切られる", closed_at is not None and 28 < closed_at < 36,
          f"closed at {closed_at:.1f} s, {texts} text frames" if closed_at else f"not closed in {deadline - t0:.0f} s")


def closed():
    try:
        socket.create_connection(("127.0.0.1", PORT), timeout=2).close()
        check("1. 受信口を閉じたら待ち受けない", False, "connected")
    except OSError as e:
        check("1. 受信口を閉じたら待ち受けない", True, str(e))


if PHASE == "idle":
    idle()
elif PHASE == "closed":
    closed()
else:
    asyncio.run({"basic": basic, "control": control, "token": token, "limits": limits}[PHASE]())
sys.exit(FAIL)
