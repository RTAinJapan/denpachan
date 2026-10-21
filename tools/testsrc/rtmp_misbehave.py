"""publish しない RTMP クライアント (rtmpd の試験用)。

    python -I tools/testsrc/rtmp_misbehave.py <host> <port> <app> flood   publish せずにデータを送り続ける (最大 4 MB)
    python -I tools/testsrc/rtmp_misbehave.py <host> <port> <app> idle    publish せずに 2 秒ごとに小さなメッセージを送る (最大 60 秒)

切られるまでに送ったバイト数と経った秒数を出す。
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


def amf_object(items: dict) -> bytes:
    out = b"\x03"
    for k, v in items.items():
        kb = k.encode()
        out += struct.pack(">H", len(kb)) + kb + amf_string(v)
    return out + b"\x00\x00\x09"


def message(payload: bytes, type_id: int, stream_id: int) -> bytes:
    # チャンクストリーム 3、fmt 0。既定のチャンクサイズ 128 で分ける
    header = b"\x03" + b"\x00\x00\x00" + struct.pack(">I", len(payload))[1:] + bytes([type_id]) + struct.pack("<I", stream_id)
    chunks = [payload[i : i + 128] for i in range(0, len(payload), 128)]
    return header + b"\xc3".join(chunks)


def main() -> int:
    host, port, app, mode = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
    sock = socket.create_connection((host, port), timeout=5)
    sock.sendall(b"\x03" + struct.pack(">II", 0, 0) + os.urandom(1528))
    s = b""
    while len(s) < 1 + 1536 * 2:
        chunk = sock.recv(65536)
        if not chunk:
            print("handshake failed")
            return 1
        s += chunk
    sock.sendall(s[1 : 1 + 1536])
    sock.sendall(message(amf_string("connect") + amf_number(1) + amf_object({"app": app, "tcUrl": f"rtmp://{host}:{port}/{app}"}), 0x14, 0))
    sock.settimeout(0.05)
    start = time.time()
    sent = 0
    try:
        if mode == "flood":
            # スクリプトデータ (type 18) を 64 KB ずつ送る
            data = message(amf_string("onTextData") + amf_string("x" * 65000), 18, 1)
            while sent < 4 * 1024 * 1024:
                sock.sendall(data)
                sent += len(data)
                try:
                    if sock.recv(65536) == b"":
                        break
                except socket.timeout:
                    pass
        else:
            data = message(amf_string("onTextData") + amf_string("ping"), 18, 1)
            while time.time() - start < 60:
                sock.sendall(data)
                sent += len(data)
                deadline = time.time() + 2
                closed = False
                while time.time() < deadline:
                    try:
                        if sock.recv(65536) == b"":
                            closed = True
                            break
                    except socket.timeout:
                        pass
                if closed:
                    break
        result = "still open"
    except (ConnectionResetError, ConnectionAbortedError, BrokenPipeError, OSError):
        result = "closed by the server"
    else:
        if sent < 4 * 1024 * 1024 and time.time() - start < 60:
            result = "closed by the server"
    print(f"{mode}: {result} after {sent} bytes, {time.time() - start:.1f} s")
    sock.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
