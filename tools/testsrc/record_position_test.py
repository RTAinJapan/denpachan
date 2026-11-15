"""録画ファイルの中の位置 (受信口の record.position) の試験の道具 (tools/testsrc/record_position_test.sh から呼ぶ)。

    python -E tools/testsrc/record_position_test.py log <port> <samples.jsonl> <stop-file>
        受信口 (WebSocket) に繋ぎ、1 秒ごとの channels から ch1 の record を 1 行ずつ書き留める。stop-file ができたら終わる
    python -E tools/testsrc/record_position_test.py check <samples.jsonl> <種類:説明>...
        書き留めた record を録画ファイルごとに分け、出てきた順に <種類:説明> と突き合わせて確かめる。
        種類: gap = 途切れをまたいだ (seconds はファイルより先へ進んでいるはず) / plain = それ以外

ファイルごとに確かめること:
  - 最後に出ていた position が、ファイルの最後のパケットの時刻 (ffprobe。ファイルの先頭からの時間) と合う
  - position は同じファイルの間は戻らない。seconds (録画を始めてからの実時間) を大きくは超えない
    (少しは超える: 送出の ffmpeg の -re は最初の 0.5 秒分をまとめて送るので、ファイルの時刻が実時間より先に進む)
  - ファイルの最初の position は小さい (新しいファイルでは 0 から数え直す)
  - gap: 最後のパケットを書いた時点で、seconds は position より 1.5 秒以上先 (途切れた時間はファイルに入らないので、
    seconds ではファイルの中を指せない)

項目ごとに OK / FAIL を出し、FAIL があれば終了コード 1。websockets を使うので -I ではなく -E で動かす (ws_test.py と同じ)。
"""

import asyncio
import json
import os
import subprocess
import sys
import time

# bash から呼ぶと標準出力が cp932 になり、日本語の行が化ける (-E では PYTHONIOENCODING も効かない)
sys.stdout.reconfigure(encoding="utf-8")

MATCH_SEC = 0.2  # position とファイルの最後の差の上限 (値は 1 秒ごとだが、送出が終わったあとの値を比べるので小さい)
FIRST_SEC = 3.0  # 新しいファイルの最初の position の上限 (worker の報告と engine の通知がそれぞれ 1 秒ごと)
AHEAD_SEC = 1.5  # position が seconds を超えてよい幅 (送出が最初にまとめて送る分。時刻の飛びをそのまま書けば秒の単位で超える)
FAIL = 0


def check(desc, ok, extra=""):
    global FAIL
    print(("OK   " if ok else "FAIL ") + desc + (f" ({extra})" if extra else ""))
    if not ok:
        FAIL = 1


async def log(port, out_path, stop_path):
    from websockets.asyncio.client import connect

    async with connect(f"ws://127.0.0.1:{port}/ws", open_timeout=5) as ws:
        with open(out_path, "w", encoding="utf-8") as out:
            while not os.path.exists(stop_path):
                msg = json.loads(await asyncio.wait_for(ws.recv(), 10))
                if msg.get("event") != "channels":
                    continue
                ch1 = next((c for c in msg.get("channels", []) if c.get("ch") == 1), {})
                out.write(json.dumps({"t": time.monotonic(), "status": ch1.get("status"), "record": ch1.get("record")},
                                     ensure_ascii=False) + "\n")
                out.flush()


def file_end(path):
    """録画ファイルの最後のパケットの時刻 (ファイルの先頭からの秒数。ffmpeg の -ss と同じ数え方)。"""
    r = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=start_time:packet=pts_time", "-of", "json", path],
                       capture_output=True, text=True, encoding="utf-8")
    j = json.loads(r.stdout or "{}")
    pts = [float(p["pts_time"]) for p in j.get("packets", []) if p.get("pts_time") not in (None, "N/A")]
    if not pts:
        return None
    start = j.get("format", {}).get("start_time")
    return max(pts) - (float(start) if start not in (None, "N/A") else min(pts))


def run_check(samples_path, labels):
    files = {}  # ファイル名 → そのファイルを録画中の record (出てきた順)
    with open(samples_path, encoding="utf-8") as f:
        for line in f:
            rec = json.loads(line).get("record") or {}
            if rec.get("state") == "recording" and rec.get("file"):
                files.setdefault(rec["file"], []).append(rec)
    check(f"録画ファイルは {len(labels)} 個", len(files) == len(labels), f"{len(files)} 個")
    for n, ((path, recs), label) in enumerate(zip(files.items(), labels), 1):
        kind, _, desc = label.partition(":")
        print(f"---- {n}. {desc}: {os.path.basename(path)}")
        end = file_end(path)
        last = recs[-1]
        seconds = last.get("seconds") or 0
        if end is None:
            check(f"{n}. 録画ファイルを読める", False, path)
            continue
        print(f"     ファイルの最後 {end:.3f} s / 最後に出ていた seconds {seconds:.1f} s (ファイルより {seconds - end:+.1f} s)")
        if any(r.get("position") is None for r in recs):
            check(f"{n}. record に position がある", False, f"record = {json.dumps(last, ensure_ascii=False)}")
            continue
        pos = [float(r["position"]) for r in recs]
        check(f"{n}a. 最後の position がファイルの最後と合う (±{MATCH_SEC} s)", abs(pos[-1] - end) <= MATCH_SEC,
              f"position {pos[-1]:.3f} s、ファイル {end:.3f} s、差 {pos[-1] - end:+.3f} s")
        check(f"{n}b. position は戻らない", all(b >= a for a, b in zip(pos, pos[1:])), " ".join(f"{p:.1f}" for p in pos))
        over = max(p - (r.get("seconds") or 0) for p, r in zip(pos, recs))
        check(f"{n}c. position は seconds を大きくは超えない (+{AHEAD_SEC} s まで)", over <= AHEAD_SEC, f"最大 {over:+.2f} s")
        check(f"{n}d. このファイルの最初の position は小さい (0 から数える)", pos[0] <= FIRST_SEC, f"{pos[0]:.2f} s")
        # 最後のパケットを書いた直後の値で、seconds と position を比べる (そのあとファイルが閉じるまでの待ち時間を入れない)
        at_end = next(r for r in recs if float(r["position"]) >= pos[-1] - 0.05)
        ahead = (at_end.get("seconds") or 0) - float(at_end["position"])
        print(f"     最後のパケットを書いた時点で、seconds は position より {ahead:+.1f} s")
        if kind == "gap":
            check(f"{n}e. seconds は途切れた分だけ position より先 (seconds ではファイルの中を指せない)", ahead >= 1.5, f"{ahead:+.1f} s")


if sys.argv[1] == "log":
    asyncio.run(log(int(sys.argv[2]), sys.argv[3], sys.argv[4]))
else:
    run_check(sys.argv[2], sys.argv[3:])
sys.exit(FAIL)
