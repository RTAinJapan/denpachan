"""1 つの RTMP 接続で、2 つのキーに続けて publish する (rtmpd の試験用。FFmpeg ではできない送り方)。

    python -I tools/testsrc/rtmp_double_publish.py <host> <port> <app> <key1> <key2>
    (key2 に - を渡すと、key1 に publish するだけで切る)

ハンドシェイク → connect → createStream → publish key1 → publish key2 → 2 秒待って切断。
メディアは送らない。サーバからの応答は読み捨てる (2 回目の publish の応答だけ表示する)。
"""

import os
import socket
import struct
import sys
import time


def amf_string(s: str) -> bytes:
    b = s.encode()
    return b"\x02" + struct.pack(">H", len(b)) + b


def amf_number(v: float) -> bytes:
    return b"\x00" + struct.pack(">d", v)


def amf_null() -> bytes:
    return b"\x05"


def amf_object(items: dict) -> bytes:
    out = b"\x03"
    for k, v in items.items():
        kb = k.encode()
        out += struct.pack(">H", len(kb)) + kb + amf_string(v)
    return out + b"\x00\x00\x09"


def command(payload: bytes, stream_id: int) -> bytes:
    # チャンクストリーム 3、fmt 0、型 20 (AMF0 コマンド)。既定のチャンクサイズ 128 で分ける
    header = b"\x03" + b"\x00\x00\x00" + struct.pack(">I", len(payload))[1:] + b"\x14" + struct.pack("<I", stream_id)
    chunks = [payload[i : i + 128] for i in range(0, len(payload), 128)]
    return header + b"\xc3".join(chunks)


def drain(sock: socket.socket, seconds: float) -> bytes:
    sock.settimeout(0.2)
    data = b""
    end = time.time() + seconds
    while time.time() < end:
        try:
            chunk = sock.recv(65536)
            if not chunk:
                break
            data += chunk
        except socket.timeout:
            pass
    return data


def main() -> int:
    host, port, app, key1, key2 = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5]
    sock = socket.create_connection((host, port), timeout=5)
    c1 = struct.pack(">II", 0, 0) + os.urandom(1528)
    sock.sendall(b"\x03" + c1)
    s = b""
    while len(s) < 1 + 1536 * 2:
        chunk = sock.recv(65536)
        if not chunk:
            print("handshake failed")
            return 1
        s += chunk
    sock.sendall(s[1 : 1 + 1536])  # C2 = S1

    tc_url = f"rtmp://{host}:{port}/{app}"
    sock.sendall(command(amf_string("connect") + amf_number(1) + amf_object({"app": app, "tcUrl": tc_url}), 0))
    drain(sock, 0.5)
    sock.sendall(command(amf_string("createStream") + amf_number(2) + amf_null(), 0))
    drain(sock, 0.5)
    sock.sendall(command(amf_string("publish") + amf_number(0) + amf_null() + amf_string(key1) + amf_string(app), 1))
    first = drain(sock, 1.0)
    print("first publish:", "NetStream.Publish.Start" if b"NetStream.Publish.Start" in first else "no start")
    if key2 == "-":  # 2 回目は送らない (中身のない publish を 1 回だけして切る)
        drain(sock, 1.0)
        sock.close()
        return 0
    sock.sendall(command(amf_string("publish") + amf_number(0) + amf_null() + amf_string(key2) + amf_string(app), 1))
    second = drain(sock, 1.0)
    codes = [c for c in (b"NetStream.Publish.Start", b"NetStream.Publish.BadConnection", b"NetStream.Publish.BadName") if c in second]
    print("second publish:", ", ".join(c.decode() for c in codes) or "no reply")
    sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
