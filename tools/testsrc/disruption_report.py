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
  - GPU のドライバが回復した: 同じ時刻に Windows のイベント (Display 4101 など、TDR) → denpachan の外。
  - モニターのつながりが変わった: 同じ時刻に Windows のイベントで、モニターの音声 (NVIDIA High Definition Audio など)
    の出口が消えた・現れた、または画面の点灯・ロック解除 (Kernel-Power 566) → denpachan の外。モニターの
    つながり直し (DisplayPort の張り直し、省電力からの復帰など) の間、GPU の HW デコードが全チャンネルそろって
    0.5〜1 秒止まる。
  - デコードが遅れた: 「decoder: decoding one ... packet took N ms」 → GPU (HW デコード) などの詰まり。
  - データが届かなかった: 受信が途絶え、明けたときに溜まっていたのは少しだけ → 送り側・回線 (denpachan の外)。
  - 判断できない: 手がかりの行が無い (この版より前のログ、debug でない、など)。

worker のログは SPDLOG_LEVEL=debug だと「arrival gap」(届く間隔による途切れ) も拾える。
Windows のイベントは、このスクリプトを回した PC のものを読む。ログをほかの PC から持ってきたときは
--no-windows-events を付ける (その PC で回せば、その PC のイベントと突き合わせられる)。
"""

import datetime
import glob
import os
import re
import subprocess
import sys

sys.stdout.reconfigure(encoding="utf-8")

args = sys.argv[1:]
since = until = None
gap_s = 2.0
use_windows_events = os.name == "nt"
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
    elif a == "--no-windows-events":
        use_windows_events = False
        i += 1
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


# Windows のイベント (この PC のもの)。1 行に「時刻 TAB 種類 TAB 中身」で出させる
WINDOWS_EVENTS_PS = r"""
$ErrorActionPreference = 'SilentlyContinue'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$s = [datetime]::ParseExact('%s', 'yyyy-MM-dd HH:mm:ss', $null)
$e = [datetime]::ParseExact('%s', 'yyyy-MM-dd HH:mm:ss', $null)
function Out($ev, $kind, $text) { "{0:yyyy-MM-dd HH:mm:ss.fff}`t{1}`t{2}" -f $ev.TimeCreated, $kind, $text }
Get-WinEvent -FilterHashtable @{LogName='Microsoft-Windows-Audio/Operational'; Id=65; StartTime=$s; EndTime=$e} | ForEach-Object {
    $x = [xml]$_.ToXml(); $d = @{}; $x.Event.EventData.Data | ForEach-Object { $d[$_.Name] = $_.'#text' }
    Out $_ 'audio' $d['DeviceName'] }
Get-WinEvent -FilterHashtable @{LogName='System'; ProviderName='Microsoft-Windows-Kernel-Power'; Id=566; StartTime=$s; EndTime=$e} | ForEach-Object {
    $r = if ($_.Message -match 'Reason\s+(\w+)') { $Matches[1] } else { '' }; Out $_ 'session' $r }
Get-WinEvent -FilterHashtable @{LogName='System'; Level=1,2,3; StartTime=$s; EndTime=$e} | Where-Object {
    $_.ProviderName -in 'Display','nvlddmkm','amdkmdag','amdkmdap','igfx','igfxn' -or ($_.ProviderName -eq 'Microsoft-Windows-Kernel-PnP' -and $_.Id -eq 1010 -and $_.Message -match 'DISPLAY') } | ForEach-Object {
    Out $_ 'gpu' ("{0} {1}" -f $_.ProviderName, $_.Id) }
"""
# モニター (HDMI・DisplayPort) の音声の出口。これが消えた・現れたら、モニターのつながりが変わった
DISPLAY_AUDIO = re.compile(r"NVIDIA High Definition Audio|Intel\(R\) Display|AMD High Definition Audio|Display Audio", re.I)
SESSION_REASON = {"InputHid": "画面の点灯 (キー・マウス)", "SessionUnlock": "ロックの解除", "SessionLock": "ロック",
                  "SxTransition": "スリープからの復帰"}


def windows_events(lo, hi):
    """lo〜hi (datetime) の Windows のイベントを [(時刻, 種類, 中身)] で返す。読めなければ空"""
    script = WINDOWS_EVENTS_PS % (lo.strftime("%Y-%m-%d %H:%M:%S"), hi.strftime("%Y-%m-%d %H:%M:%S"))
    try:
        out = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
                             capture_output=True, timeout=300).stdout
    except (OSError, subprocess.TimeoutExpired):
        return []
    rows = []
    for line in out.decode("utf-8", errors="replace").splitlines():
        parts = line.strip("﻿").split("\t")
        if len(parts) == 3:
            try:
                rows.append((datetime.datetime.strptime(parts[0], "%Y-%m-%d %H:%M:%S.%f"), parts[1], parts[2]))
            except ValueError:
                pass
    return rows


win_events = []
if use_windows_events and incidents:
    win_events = windows_events(incidents[0][0][0] - datetime.timedelta(seconds=10),
                                incidents[-1][-1][0] + datetime.timedelta(seconds=10))


def windows_near(inc):
    """件の前 3 秒〜後 5 秒の Windows のイベント (ロック解除のイベントは数秒遅れて書かれる)"""
    lo, hi = inc[0][0] - datetime.timedelta(seconds=3), inc[-1][0] + datetime.timedelta(seconds=5)
    return [w for w in win_events if lo <= w[0] <= hi]


def describe(w):
    t, kind, text = w
    if kind == "audio":
        what = f"音声の出口の状態が変わった: {text}" + (" (モニター)" if DISPLAY_AUDIO.search(text) else "")
    elif kind == "session":
        what = f"セッションの移り変わり: {SESSION_REASON.get(text, text)}"
    else:
        what = f"GPU・画面のドライバ: {text}"
    return f"    {t:%H:%M:%S.%f}"[:-3] + f" [Windows] {what}"


def verdict(inc):
    stalls = {e[3][0] for e in inc if e[2] == "stall"}
    rx = [(int(e[3][0]), int(e[3][1])) for e in inc if e[2] in ("srt_rx", "rtmp_rx")]
    waited = [g for g, s in rx if s >= g / 2]
    starved = [g for g, s in rx if s < g / 2]
    decode = [e for e in inc if e[2] == "decode"]
    near = windows_near(inc)
    gpu = [w for w in near if w[1] == "gpu"]
    display = [w for w in near if (w[1] == "audio" and DISPLAY_AUDIO.search(w[2])) or w[1] == "session"]
    if len(stalls) >= 2:
        return "PC 全体が止まった (denpachan の外: " + ", ".join(sorted(stalls)) + ")"
    if stalls:
        return "プロセスが止まった (denpachan: " + ", ".join(sorted(stalls)) + ")"
    if gpu:
        return "GPU のドライバが回復した (denpachan の外: " + ", ".join(sorted({w[2] for w in gpu})) + ")"
    if display:
        return "モニターのつながりが変わった (denpachan の外: その間 GPU の HW デコードが止まる)"
    if waited:
        return "受信が読めていなかった (データは届いていた。denpachan の受信が止まった)"
    if decode:
        return "デコードが遅れた (GPU などの詰まり)"
    if starved:
        return "データが届かなかった (送り側・回線。denpachan の外)"
    return "判断できない (手がかりの行が無い)"


print(f"ログ: {log_dir}  ({len(files())} ファイル、手がかりの行 {len(events)})")
if use_windows_events:
    print(f"Windows のイベント (この PC のもの): {len(win_events)} 件")
print(f"途切れ・取り直しのあった件: {len(incidents)}")
counts = {}
for inc in incidents:
    t0 = inc[0][0]
    chans = sorted({e[1] for e in inc if e[2] in ("outage", "gap", "reanchor")})
    v = verdict(inc)
    counts[v.split(" (")[0]] = counts.get(v.split(" (")[0], 0) + 1
    print()
    print(f"■ {t0:%Y-%m-%d %H:%M:%S}  乱れたチャンネル: {', '.join(chans)}  → {v}")
    lines = [(e[0], f"    {e[0]:%H:%M:%S.%f}"[:-3] + f" [{e[1]}] {e[4]}") for e in inc]
    near = windows_near(inc)
    # 音声の出口は 1 回のつながり直しで何十行も出るので、名前ごとに最初の 1 行だけ
    seen = set()
    for w in near:
        key = (w[1], w[2])
        if w[1] == "audio" and key in seen:
            continue
        seen.add(key)
        lines.append((w[0], describe(w)))
    for _, text in sorted(lines, key=lambda x: x[0]):
        print(text)
print()
print("== 推定の内訳")
for k, n in sorted(counts.items(), key=lambda x: -x[1]):
    print(f"  {k}: {n} 件")
