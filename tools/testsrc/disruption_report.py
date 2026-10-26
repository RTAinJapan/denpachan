"""乱れ (途切れ) が denpachan 由来かを、ログから見分ける (設計書 §6.3「乱れの調べ方」)。

    python -E tools/testsrc/disruption_report.py [ログのフォルダ] [--since "YYYY-MM-DD HH:MM:SS"] [--until ...] [--gap 2]

ログのフォルダの既定は %LOCALAPPDATA%\\denpachan\\logs (engine.log、chN.log、rtmpd.log と、回転した .1〜.5)。
長時間試験の run フォルダ (out/check/soak/run2-*) も渡せる。Microsoft Store 版の Python は AppData を仮想化していて
本当のログが見えないので、そのときはログのフォルダをほかの場所に写してから渡す。

途切れ・時間軸の取り直しの行と、その前後の手がかりを、時刻が近いもの (--gap 秒以内) でまとめて 1 件にし、
件ごとにどこで止まったかを推定する:

  - PC 全体が止まった: 2 つ以上のプロセス (engine・worker・rtmpd) で同時に「stall: ... did not run」
    → denpachan の外 (OS・ドライバ・ほかのプロセス・電源管理)。
  - worker / rtmpd が止まった: 1 つのプロセスだけ stall → そのプロセス (denpachan)。
  - 受信が読めていなかった: 受信が途絶えた後、途絶えた長さの半分以上が一度に読めた
    (「SRT: no data ... then ... at once (about N ms ...)」「RTMP: no data ... waiting in TCP (about N ms ...)」)
    → データは届いていたのに denpachan が読めなかった。
  - デコードが遅れた: 「decoder: decoding one ... packet took N ms」 → GPU (HW デコード) などの詰まり。
  - データが届かなかった: 受信が途絶え、明けたときに溜まっていたのは少しだけ → 送り側・回線 (denpachan の外)。
  - 判断できない: 手がかりの行が無い (この版より前のログ、debug でない、など)。

worker のログは SPDLOG_LEVEL=debug だと「arrival gap」(届く間隔による途切れ) も拾える。
"""

import datetime
import glob
import os
import re
import sys

sys.stdout.reconfigure(encoding="utf-8")

args = sys.argv[1:]
since = until = None
gap_s = 2.0
paths = []
i = 0
while i < len(args):
    a = args[i]
    if a == "--since":
        since = datetime.datetime.strptime(args[i + 1], "%Y-%m-%d %H:%M:%S")
        i += 2
    elif a == "--until":
        until = datetime.datetime.strptime(args[i + 1], "%Y-%m-%d %H:%M:%S")
        i += 2
    elif a == "--gap":
        gap_s = float(args[i + 1])
        i += 2
    else:
        paths.append(a)
        i += 1
log_dir = paths[0] if paths else os.path.join(os.environ.get("LOCALAPPDATA", ""), "denpachan", "logs")
# Microsoft Store 版の Python は AppData を仮想化していて、本当のログのフォルダが見えない (別の空のフォルダが見える)。
# そのときは、ログをほかのフォルダに写してから渡す
if "WindowsApps" in sys.executable and os.path.abspath(log_dir).lower().startswith(
    os.path.abspath(os.environ.get("LOCALAPPDATA", "?")).lower()
):
    print("注意: Microsoft Store 版の Python では AppData の下のログが見えないことがあります。"
          "ログのフォルダをほかの場所に写して、その場所を渡してください。")

LINE = re.compile(r"^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}) \[([^\]]+)\] (\w+) (.*)$")
PATTERNS = [
    # (種類, 正規表現)。数値は group(1)、2 つ目があれば group(2)
    ("outage", re.compile(r"playout: (?:audio|video) arrived ([\d.]+) ms late \((?:outage|repeated outages)\)")),
    ("gap", re.compile(r"playout: (?:audio|video) arrival gap (\d+) ms \(outage\)")),
    ("reanchor", re.compile(r"playout: video cannot keep up with the delay \((\d+) ms")),
    ("stall", re.compile(r"stall: (.+?) did not run for (\d+) ms")),
    ("srt_rx", re.compile(r"SRT: no data for (\d+) ms, then \d+ KB at once \(about (\d+) ms")),
    ("rtmp_rx", re.compile(r"RTMP: no data from \S+ for (\d+) ms, then \d+ KB waiting in TCP \(about (\d+) ms")),
    ("decode", re.compile(r"decoder: decoding one (\w+) packet took (\d+) ms")),
]


def files():
    out = []
    for base in ["engine", "rtmpd"] + [f"ch{n}" for n in range(1, 9)]:
        for k in (5, 4, 3, 2, 1):
            out += glob.glob(os.path.join(log_dir, f"{base}.{k}.log"))
        out += glob.glob(os.path.join(log_dir, f"{base}.log"))
    return out


events = []
for path in files():
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = LINE.match(line)
            if not m:
                continue
            t = datetime.datetime.strptime(m[1], "%Y-%m-%d %H:%M:%S.%f")
            if (since and t < since) or (until and t > until):
                continue
            who, msg = m[2], m[4]
            for kind, pat in PATTERNS:
                p = pat.search(msg)
                if p:
                    events.append((t, who, kind, p.groups(), msg))
                    break
events.sort(key=lambda e: e[0])

# 時刻の近いものをまとめる (途切れ・取り直しを含むものだけを 1 件として出す)
incidents, cur = [], []
for e in events:
    if cur and (e[0] - cur[-1][0]).total_seconds() > gap_s:
        incidents.append(cur)
        cur = []
    cur.append(e)
if cur:
    incidents.append(cur)
incidents = [inc for inc in incidents if any(e[2] in ("outage", "gap", "reanchor") for e in inc)]


def verdict(inc):
    stalls = {e[3][0] for e in inc if e[2] == "stall"}
    rx = [(int(e[3][0]), int(e[3][1])) for e in inc if e[2] in ("srt_rx", "rtmp_rx")]
    waited = [g for g, s in rx if s >= g / 2]
    starved = [g for g, s in rx if s < g / 2]
    decode = [e for e in inc if e[2] == "decode"]
    if len(stalls) >= 2:
        return "PC 全体が止まった (denpachan の外: " + ", ".join(sorted(stalls)) + ")"
    if stalls:
        return "プロセスが止まった (denpachan: " + ", ".join(sorted(stalls)) + ")"
    if waited:
        return "受信が読めていなかった (データは届いていた。denpachan の受信が止まった)"
    if decode:
        return "デコードが遅れた (GPU などの詰まり)"
    if starved:
        return "データが届かなかった (送り側・回線。denpachan の外)"
    return "判断できない (手がかりの行が無い)"


print(f"ログ: {log_dir}  ({len(files())} ファイル、手がかりの行 {len(events)})")
print(f"途切れ・取り直しのあった件: {len(incidents)}")
counts = {}
for inc in incidents:
    t0 = inc[0][0]
    chans = sorted({e[1] for e in inc if e[2] in ("outage", "gap", "reanchor")})
    v = verdict(inc)
    counts[v.split(" (")[0]] = counts.get(v.split(" (")[0], 0) + 1
    print()
    print(f"■ {t0:%Y-%m-%d %H:%M:%S}  乱れたチャンネル: {', '.join(chans)}  → {v}")
    for t, who, kind, g, msg in inc:
        print(f"    {t:%H:%M:%S.%f}"[:-3] + f" [{who}] {msg}")
print()
print("== 推定の内訳")
for k, n in sorted(counts.items(), key=lambda x: -x[1]):
    print(f"  {k}: {n} 件")
