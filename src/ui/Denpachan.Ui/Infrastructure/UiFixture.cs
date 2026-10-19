using System.IO;
using System.Text.Json;
using System.Text.Json.Nodes;
using Denpachan.Ui.ViewModels;

namespace Denpachan.Ui.Infrastructure;

/// <summary>
/// 試験用: engine に繋がずに、ファイルに書いた設定と状態を画面に流し込む (`--fixture PATH`)。
/// CI で画面のスクリーンショットを撮る (tools/ui_screenshots/) ために使う。
///
/// ファイルの形:
///   connected   engine に繋がっているふりをするか (既定 true)
///   general     全体の設定 (patchGeneral と同じ形。省略時は既定値)
///   channels    チャンネルの設定の配列 (getConfig の channels と同じ形。書いた項目だけ既定値に上書き)
///   state       engine の state 通知の data と同じ形
///   repeat      state を何回流すか (グラフの履歴を作る。ビットレート・遅延・RTT は少し揺らす)
///   counters    [{ch, path, from}] 累積カウンタを repeat の間に from から state の値まで増やす (直近 1 分の増分で色を出す項目の試験)
///   testPattern プレビューにカラーバーを出すチャンネル番号の配列
///   locked      チャンネルごとの操作ロック
///   select      選ぶチャンネル
///   events      [{text, warning}] イベントログ
///   expect      画面の状態の期待値 (下の Check)。合わなければ終了コード 1
/// </summary>
public static class UiFixture
{
    public static JsonNode Load(string path) =>
        JsonNode.Parse(File.ReadAllText(path)) ?? throw new InvalidDataException($"fixture が空です: {path}");

    public static void Apply(MainViewModel vm, JsonNode fixture)
    {
        vm.FixtureMode = true;
        vm.SetConnectedFromFixture(fixture.Bool("connected", true));
        var general = DefaultGeneral();
        if (fixture.At("general") is JsonObject g) Merge(general, g);
        vm.SetGeneralConfigFromFixture(general);

        if (fixture.At("channels") is JsonArray channels)
        {
            for (var i = 0; i < channels.Count && i < vm.Channels.Count; i++)
            {
                var id = channels[i].Int("id", i + 1);
                if (id < 1 || id > vm.Channels.Count) continue;
                var config = DefaultChannelConfig(id);
                if (channels[i] is JsonObject c) Merge(config, c);
                vm.Channels[id - 1].ApplyConfig(config);
            }
        }

        if (fixture.At("state") is JsonObject state)
        {
            var repeat = Math.Max(0, fixture.Int("repeat"));
            var counters = fixture.At("counters") as JsonArray;
            for (var i = 0; i < repeat; i++)
            {
                var step = (JsonObject)state.DeepClone();
                Jitter(step, i);
                if (counters != null) RampCounters(step, state, counters, (double)i / Math.Max(1, repeat));
                vm.ApplyStateFromFixture(step);
            }
            vm.ApplyStateFromFixture(state);
        }

        if (fixture.At("testPattern") is JsonArray pattern)
        {
            foreach (var n in pattern)
            {
                var id = n?.GetValue<int>() ?? 0;
                if (id >= 1 && id <= vm.Channels.Count) vm.Channels[id - 1].ShowTestPattern(id);
            }
        }
        // 画面の設定 (ui.json) に左右されないよう、ロックとプレビューは必ずファイルの値 (既定: ロックなし、プレビューあり) にする
        var locked = fixture.At("locked") as JsonArray;
        for (var i = 0; i < vm.Channels.Count; i++)
        {
            vm.Channels[i].IsLocked = locked != null && i < locked.Count && (locked[i]?.GetValue<bool>() ?? false);
            vm.Channels[i].PreviewEnabled = true;
        }
        if (fixture.At("events") is JsonArray events)
        {
            foreach (var e in events) vm.Log(e.Str("text"), e.Bool("warning"));
        }
        var select = fixture.Int("select", 1);
        if (select >= 1 && select <= vm.Channels.Count) vm.Selected = vm.Channels[select - 1];
    }

    /// <summary>expect と画面の状態を比べ、合わない項目を返す (空なら合格)</summary>
    public static List<string> Check(MainViewModel vm, JsonNode? expect)
    {
        var failures = new List<string>();
        if (expect == null) return failures;
        void Expect(string label, object? actual, JsonNode? wanted)
        {
            if (wanted == null) return;
            // bool は JSON の true/false と C# の True/False を同じに扱う
            var a = actual is bool ab ? (ab ? "true" : "false") : actual?.ToString() ?? "";
            var w = wanted is JsonValue v && v.TryGetValue<string>(out var s) ? s : wanted.ToJsonString();
            if (!string.Equals(a, w, StringComparison.Ordinal)) failures.Add($"{label}: 期待 {w} / 実際 {a}");
        }
        Expect("connected", vm.Connected, expect["connected"]);
        Expect("ffmpegMissing", vm.FfmpegMissing, expect["ffmpegMissing"]);
        if (expect.At("channels") is JsonArray channels)
        {
            for (var i = 0; i < channels.Count && i < vm.Channels.Count; i++)
            {
                var e = channels[i];
                if (e == null) continue;
                var ch = vm.Channels[i];
                var p = $"ch{i + 1}.";
                Expect(p + "statusText", ch.StatusText, e["statusText"]);
                Expect(p + "statusKind", ch.StatusKind, e["statusKind"]);
                Expect(p + "enabled", ch.Enabled, e["enabled"]);
                Expect(p + "softwareDecode", ch.SoftwareDecode, e["softwareDecode"]);
                Expect(p + "transportKind", ch.TransportKind, e["transportKind"]);
                Expect(p + "audioKind", ch.AudioKind, e["audioKind"]);
                Expect(p + "recordKind", ch.RecordKind, e["recordKind"]);
                Expect(p + "isLocked", ch.IsLocked, e["isLocked"]);
                Expect(p + "previewMessage", ch.PreviewMessage, e["previewMessage"]);
            }
        }
        return failures;
    }

    /// <summary>画面の状態を書き出す (スクリーンショットと並べて置く)</summary>
    public static string Report(MainViewModel vm, IReadOnlyList<string> failures)
    {
        var report = new JsonObject
        {
            ["result"] = failures.Count == 0 ? "ok" : "failed",
            ["failures"] = new JsonArray(failures.Select(f => (JsonNode)f).ToArray()),
            ["connected"] = vm.Connected,
            ["ffmpegMissing"] = vm.FfmpegMissing,
            ["systemText"] = vm.SystemText,
            ["channels"] = new JsonArray(vm.Channels.Select(ch => (JsonNode)new JsonObject
            {
                ["name"] = ch.Name,
                ["statusText"] = ch.StatusText,
                ["statusKind"] = ch.StatusKind,
                ["enabled"] = ch.Enabled,
                ["softwareDecode"] = ch.SoftwareDecode,
                ["transportKind"] = ch.TransportKind,
                ["audioKind"] = ch.AudioKind,
                ["recordKind"] = ch.RecordKind,
                ["isLocked"] = ch.IsLocked,
                ["previewMessage"] = ch.PreviewMessage,
                ["videoLine"] = ch.VideoLine,
                ["transportLine"] = ch.TransportLine,
                ["audioLine"] = ch.AudioLine,
                ["recordLine"] = ch.RecordLine,
            }).ToArray()),
        };
        return report.ToJsonString(new JsonSerializerOptions { WriteIndented = true, Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping });
    }

    // ---- 既定値 (engine の default_config と同じ) ----

    private static JsonObject DefaultGeneral() => new()
    {
        ["stallMs"] = 2000,
        ["audioMarginMs"] = 10,
        ["avSync"] = true,
        ["ffmpegPath"] = "",
        ["rtmpServer"] = new JsonObject { ["bind"] = "0.0.0.0", ["port"] = 1935, ["idleTimeoutMs"] = 5000 },
    };

    private static JsonObject DefaultChannelConfig(int id) => new()
    {
        ["id"] = id,
        ["name"] = $"CH{id}",
        ["enabled"] = false,
        ["source"] = new JsonObject
        {
            ["protocol"] = "srt", ["mode"] = "listener", ["host"] = "0.0.0.0", ["port"] = 9000 + id, ["latencyMs"] = 120,
            ["pbkeylen"] = 16, ["streamId"] = "", ["app"] = "live", ["hasPassphrase"] = false, ["key"] = "", ["url"] = "",
        },
        ["playoutBufferMs"] = 100,
        ["vcam"] = id,
        ["audio"] = new JsonObject
        {
            ["device"] = "", ["startChannel"] = 1 + 2 * (id - 1), ["mode"] = "shared", ["offsetMs"] = 0, ["mute"] = false, ["fade"] = true,
        },
        ["record"] = new JsonObject
        {
            ["enabled"] = false, ["dir"] = "", ["template"] = "{ch} - {YYYY}{MM}/{DD} {hh}-{mm}-{ss}", ["container"] = "mkv",
            ["mode"] = "passthrough", ["gapTimeoutSec"] = 10, ["maxSegmentMin"] = 0, ["invalidCharReplacement"] = "-", ["minFreeGb"] = 5,
            ["encode"] = new JsonObject
            {
                ["video"] = new JsonObject
                {
                    ["codec"] = "h264", ["encoder"] = "auto", ["rc"] = "cbr", ["bitrateKbps"] = 12000, ["qp"] = 23, ["preset"] = "p4", ["gopSec"] = 2,
                },
                ["audio"] = new JsonObject { ["bitrateKbps"] = 192 },
            },
        },
    };

    // ---- 補助 ----

    /// <summary>patch の項目で target を上書きする (オブジェクトは中まで、配列と値は丸ごと)</summary>
    private static void Merge(JsonObject target, JsonObject patch)
    {
        foreach (var (key, value) in patch)
        {
            if (value is JsonObject po && target[key] is JsonObject to) Merge(to, po);
            else target[key] = value?.DeepClone();
        }
    }

    /// <summary>グラフが平らにならないよう、ビットレート・遅延・RTT を少し揺らす</summary>
    private static void Jitter(JsonObject state, int i)
    {
        if (state["channels"] is not JsonArray channels) return;
        foreach (var ch in channels)
        {
            if (ch is not JsonObject c) continue;
            var k = 1 + 0.08 * Math.Sin(i / 7.0) + 0.03 * Math.Sin(i / 2.3);
            Scale(c["video"], "bitrateMbps", k);
            Scale(c["video"], "delayMs", 1 + 0.15 * Math.Sin(i / 5.0));
            Scale(c["srt"], "rttMs", 1 + 0.2 * Math.Sin(i / 3.0));
            Scale(c["tcp"], "rttMs", 1 + 0.2 * Math.Sin(i / 3.0));
        }
    }

    private static void Scale(JsonNode? obj, string key, double k)
    {
        if (obj is JsonObject o && o[key] is JsonValue v && v.TryGetValue<double>(out var d)) o[key] = Math.Round(d * k, 2);
    }

    /// <summary>counters の項目を from から最終値まで t (0..1) の割合で進める</summary>
    private static void RampCounters(JsonObject step, JsonObject final, JsonArray counters, double t)
    {
        foreach (var counter in counters)
        {
            var ch = counter.Int("ch");
            var path = counter.Str("path").Split('.');
            if (path.Length == 0 || step["channels"] is not JsonArray steps || final["channels"] is not JsonArray finals) continue;
            var s = steps.FirstOrDefault(c => c.Int("id") == ch) as JsonObject;
            var f = finals.FirstOrDefault(c => c.Int("id") == ch);
            if (s == null || f == null) continue;
            JsonObject? parent = s;
            JsonNode? target = f;
            for (var i = 0; i < path.Length - 1 && parent != null; i++)
            {
                parent = parent[path[i]] as JsonObject;
                target = target?[path[i]];
            }
            if (parent == null || target?[path[^1]] is not JsonValue fv || !fv.TryGetValue<double>(out var to)) continue;
            var from = counter.Num("from");
            parent[path[^1]] = Math.Floor(from + (to - from) * t);
        }
    }
}
