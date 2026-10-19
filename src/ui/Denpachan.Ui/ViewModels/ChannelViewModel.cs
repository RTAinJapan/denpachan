using System.IO;
using System.Text.Json.Nodes;
using System.Windows.Input;
using System.Windows.Media.Imaging;
using Denpachan.Ui.Engine;
using Denpachan.Ui.Infrastructure;

namespace Denpachan.Ui.ViewModels;

public sealed record Option<T>(T Value, string Label);

public sealed record DetailRow(string Label, string Value);

/// <summary>1 チャンネル分の表示と操作</summary>
public sealed class ChannelViewModel : ObservableObject, IDisposable
{
    private const int HistoryLength = 600;  // 10 分

    private readonly MainViewModel _main;
    private readonly PreviewReader _preview;
    private readonly List<double> _bitrate = [];
    private readonly List<double> _delay = [];
    private readonly List<double> _rtt = [];
    private bool _applying;  // 設定を画面に反映している間は、変更を engine に送らない

    // 前回の状態 (イベントログ用)
    private long _lastRestarts = -1;
    private string _lastVideoState = "";
    private string _lastUnreachableReason = "";
    private bool? _lastConnected;
    private string _lastWorkerState = "";

    public ChannelViewModel(MainViewModel main, int id)
    {
        _main = main;
        Id = id;
        _name = $"CH{id}";
        _preview = new PreviewReader(id);
        StartStopCommand = new RelayCommand(() => _ = StartStopAsync(), () => _main.Connected);
        MuteCommand = new RelayCommand(() => _ = _main.CallAsync("setMute", new JsonObject { ["ch"] = Id, ["mute"] = !Mute }),
                                       () => _main.Connected);
        ResetCamCommand = new RelayCommand(() => _ = _main.CallAsync("resetCam", new JsonObject { ["cam"] = Vcam }),
                                           () => _main.Connected && Vcam > 0);
        SettingsCommand = new RelayCommand(() => _main.OpenChannelSettings(this), () => _main.Connected && Config != null);
        RecordCommand = new RelayCommand(() => _ = _main.CallAsync("setRecording", new JsonObject { ["ch"] = Id, ["enabled"] = !RecordEnabled }),
                                         () => _main.Connected);
        PreviewCommand = new RelayCommand(() => PreviewEnabled = !PreviewEnabled);
    }

    public int Id { get; }
    public JsonNode? Config { get; private set; }

    public ICommand StartStopCommand { get; }
    public ICommand MuteCommand { get; }
    public ICommand ResetCamCommand { get; }
    public ICommand SettingsCommand { get; }
    public ICommand PreviewCommand { get; }
    public ICommand RecordCommand { get; }

    // ---- 録画 ----

    private bool _recordEnabled;
    public bool RecordEnabled { get => _recordEnabled; private set => SetProperty(ref _recordEnabled, value); }

    private string _recordKind = "off";
    /// <summary>off / waiting / recording / error</summary>
    public string RecordKind { get => _recordKind; private set => SetProperty(ref _recordKind, value); }

    private string _recordLine = "";
    public string RecordLine { get => _recordLine; private set => SetProperty(ref _recordLine, value); }

    private string _name;
    public string Name { get => _name; private set => SetProperty(ref _name, value); }

    private bool _enabled;
    public bool Enabled { get => _enabled; private set { if (SetProperty(ref _enabled, value)) OnPropertyChanged(nameof(StartStopLabel)); } }
    public string StartStopLabel => Enabled ? "停止" : "開始";

    private string _statusKind = "stopped";
    /// <summary>stopped / starting / waiting / live / hold / restarting</summary>
    public string StatusKind { get => _statusKind; private set => SetProperty(ref _statusKind, value); }

    private string _statusText = "停止";
    public string StatusText { get => _statusText; private set => SetProperty(ref _statusText, value); }

    private string _sourceSummary = "";
    public string SourceSummary { get => _sourceSummary; private set => SetProperty(ref _sourceSummary, value); }

    private string _videoLine = "";
    public string VideoLine { get => _videoLine; private set => SetProperty(ref _videoLine, value); }

    private string _transportLine = "";
    public string TransportLine { get => _transportLine; private set => SetProperty(ref _transportLine, value); }

    /// <summary>こちらから接続しに行く方式 (RTMP pull / SRT caller) で繋がらない理由 (繋がっていれば空)</summary>
    public string UnreachableReason { get; private set; } = "";

    private string _audioLine = "";
    public string AudioLine { get => _audioLine; private set => SetProperty(ref _audioLine, value); }

    // ---- 映像も音も出ているが劣化している状態 (タイルで色を変える) ----

    private bool _softwareDecode;
    /// <summary>受信中に HW デコードが使えず CPU でデコードしている (HW のときは何も出さない)</summary>
    public bool SoftwareDecode { get => _softwareDecode; private set => SetProperty(ref _softwareDecode, value); }

    private string _transportKind = "ok";
    /// <summary>ok / warn (遅延が足りない可能性、再送あり) / error (取りこぼし)</summary>
    public string TransportKind { get => _transportKind; private set => SetProperty(ref _transportKind, value); }

    private string _audioKind = "ok";
    /// <summary>ok / warn (直近 1 分に音切れ・合わせ直し・抜け) / error (デバイスを開けない)</summary>
    public string AudioKind { get => _audioKind; private set => SetProperty(ref _audioKind, value); }

    // 直近 1 分の増分 (累積カウンタから)
    private static readonly TimeSpan WarnWindow = TimeSpan.FromSeconds(60);
    private readonly RateTracker _audioDropouts = new(WarnWindow);  // 音切れ + 合わせ直し
    private readonly RateTracker _audioGaps = new(WarnWindow);      // worker 側の音声の抜け
    private readonly RateTracker _srtDropped = new(WarnWindow);
    private readonly RateTracker _tcpRetrans = new(WarnWindow);

    private string _lastEvent = "";
    public string LastEvent { get => _lastEvent; private set => SetProperty(ref _lastEvent, value); }

    private double _peakL = -100;
    public double PeakL { get => _peakL; private set => SetProperty(ref _peakL, value); }

    private double _peakR = -100;
    public double PeakR { get => _peakR; private set => SetProperty(ref _peakR, value); }

    private double[] _bitrateHistory = [];
    public double[] BitrateHistory { get => _bitrateHistory; private set => SetProperty(ref _bitrateHistory, value); }

    private double[] _delayHistory = [];
    public double[] DelayHistory { get => _delayHistory; private set => SetProperty(ref _delayHistory, value); }

    private double[] _rttHistory = [];
    public double[] RttHistory { get => _rttHistory; private set => SetProperty(ref _rttHistory, value); }

    private IReadOnlyList<DetailRow> _details = [];
    public IReadOnlyList<DetailRow> Details { get => _details; private set => SetProperty(ref _details, value); }

    private bool _isSelected;
    public bool IsSelected { get => _isSelected; set => SetProperty(ref _isSelected, value); }

    private bool _isLocked;
    /// <summary>操作ロック (誤操作の防止)。タイルのボタンとプルダウンを使えなくする。</summary>
    public bool IsLocked
    {
        get => _isLocked;
        set
        {
            if (!SetProperty(ref _isLocked, value)) return;
            _main.SaveUiSettings();
        }
    }

    // ---- プレビュー ----

    public WriteableBitmap PreviewBitmap => _preview.Bitmap;

    private bool _previewEnabled = true;
    public bool PreviewEnabled
    {
        get => _previewEnabled;
        set
        {
            if (!SetProperty(ref _previewEnabled, value)) return;
            UpdatePreviewMessage();
            _main.SaveUiSettings();
        }
    }

    private bool _previewHasFrame;
    public bool PreviewHasFrame { get => _previewHasFrame; private set => SetProperty(ref _previewHasFrame, value); }

    private string _previewMessage = "";
    public string PreviewMessage { get => _previewMessage; private set => SetProperty(ref _previewMessage, value); }

    public void PollPreview()
    {
        if (!PreviewEnabled) return;
        if (_preview.Poll()) PreviewHasFrame = true;
    }

    /// <summary>試験用: プレビューにカラーバーを出す (worker が無くても画面を確かめられるように)</summary>
    public void ShowTestPattern(int seed)
    {
        var bmp = _preview.Bitmap;
        int w = bmp.PixelWidth, h = bmp.PixelHeight;
        var px = new byte[w * h * 4];
        (byte R, byte G, byte B)[] bars = [(192, 192, 192), (192, 192, 0), (0, 192, 192), (0, 192, 0), (192, 0, 192), (192, 0, 0), (0, 0, 192)];
        for (var y = 0; y < h; y++)
        {
            for (var x = 0; x < w; x++)
            {
                var bar = bars[(x * bars.Length / w + seed) % bars.Length];
                var dark = y >= h * 3 / 4 ? 0.25 : 1.0;  // 下 1/4 は暗く
                var o = (y * w + x) * 4;
                px[o] = (byte)(bar.B * dark);
                px[o + 1] = (byte)(bar.G * dark);
                px[o + 2] = (byte)(bar.R * dark);
                px[o + 3] = 255;
            }
        }
        bmp.WritePixels(new System.Windows.Int32Rect(0, 0, w, h), px, w * 4, 0);
        PreviewHasFrame = true;
        UpdatePreviewMessage();
    }

    private void UpdatePreviewMessage()
    {
        PreviewMessage = !PreviewEnabled ? "プレビュー オフ" : !Enabled ? "停止中" : PreviewHasFrame ? "" : "NO SIGNAL";
    }

    // ---- ルーティング (プルダウン) ----

    public static IReadOnlyList<Option<int>> VcamOptions { get; } =
        [new(0, "なし"), new(1, "Cam 1"), new(2, "Cam 2"), new(3, "Cam 3"), new(4, "Cam 4")];

    private int _vcam;
    public int Vcam
    {
        get => _vcam;
        set
        {
            var old = _vcam;
            if (!SetProperty(ref _vcam, value) || _applying) return;
            _ = PatchOrRevertAsync(new JsonObject { ["vcam"] = value }, () => Vcam = old);
        }
    }

    private IReadOnlyList<string> _audioDeviceChoices = ["なし"];
    public IReadOnlyList<string> AudioDeviceChoices { get => _audioDeviceChoices; private set => SetProperty(ref _audioDeviceChoices, value); }

    private string _audioDevice = "なし";
    public string AudioDevice
    {
        get => _audioDevice;
        set
        {
            if (value == null) return;
            var old = _audioDevice;
            if (!SetProperty(ref _audioDevice, value)) return;
            UpdateStartChannelOptions();
            if (_applying) return;
            var device = value == "なし" ? "" : value;
            _ = PatchOrRevertAsync(new JsonObject { ["audio"] = new JsonObject { ["device"] = device } }, () => AudioDevice = old);
        }
    }

    private IReadOnlyList<Option<int>> _startChannelOptions = [new(1, "1-2")];
    public IReadOnlyList<Option<int>> StartChannelOptions { get => _startChannelOptions; private set => SetProperty(ref _startChannelOptions, value); }

    private int _audioStartChannel = 1;
    public int AudioStartChannel
    {
        get => _audioStartChannel;
        set
        {
            var old = _audioStartChannel;
            if (!SetProperty(ref _audioStartChannel, value) || _applying) return;
            _ = PatchOrRevertAsync(new JsonObject { ["audio"] = new JsonObject { ["startChannel"] = value } }, () => AudioStartChannel = old);
        }
    }

    private bool _mute;
    public bool Mute { get => _mute; private set { if (SetProperty(ref _mute, value)) OnPropertyChanged(nameof(MuteLabel)); } }
    public string MuteLabel => Mute ? "ミュート中" : "ミュート";

    public void RefreshAudioDeviceChoices()
    {
        var choices = new List<string> { "なし" };
        choices.AddRange(_main.AudioDevices.Select(d => d.Name));
        if (_audioDevice != "なし" && !choices.Contains(_audioDevice)) choices.Add(_audioDevice);  // 設定にあるが見つからないデバイス
        _applying = true;
        AudioDeviceChoices = choices;
        OnPropertyChanged(nameof(AudioDevice));
        UpdateStartChannelOptions();
        _applying = false;
    }

    private void UpdateStartChannelOptions()
    {
        var channels = _main.AudioDevices.FirstOrDefault(d => d.Name == _audioDevice)?.Channels ?? 2;
        channels = Math.Clamp(channels, 2, 64);
        var options = new List<Option<int>>();
        for (var start = 1; start + 1 <= channels; start += 2) options.Add(new(start, $"{start}-{start + 1}"));
        if (!options.Any(o => o.Value == _audioStartChannel)) options.Add(new(_audioStartChannel, $"{_audioStartChannel}-{_audioStartChannel + 1}"));
        var applying = _applying;
        _applying = true;
        StartChannelOptions = options;
        OnPropertyChanged(nameof(AudioStartChannel));
        _applying = applying;
    }

    private async Task PatchOrRevertAsync(JsonObject patch, Action revert)
    {
        if (!await _main.PatchChannelAsync(Id, patch))
        {
            _applying = true;
            revert();
            _applying = false;
        }
    }

    private async Task StartStopAsync() =>
        await _main.CallAsync(Enabled ? "stopChannel" : "startChannel", new JsonObject { ["ch"] = Id });

    // ---- engine から届いた設定・状態の反映 ----

    public void ApplyConfig(JsonNode config)
    {
        Config = config.DeepClone();
        _applying = true;
        Name = config.Str("name", $"CH{Id}");
        Enabled = config.Bool("enabled");
        Vcam = config.Int("vcam");
        var audio = config.At("audio");
        _audioDevice = audio.Str("device") is { Length: > 0 } d ? d : "なし";
        _audioStartChannel = audio.Int("startChannel", 1);
        Mute = audio.Bool("mute");
        RecordEnabled = config.At("record").Bool("enabled");
        SourceSummary = DescribeSource(config.At("source"));
        _applying = false;
        RefreshAudioDeviceChoices();
        UpdatePreviewMessage();
    }

    public static string DescribeSource(JsonNode? s)
    {
        var protocol = s.Str("protocol", "srt");
        var mode = s.Str("mode");
        if (protocol == "rtmp")
        {
            if (mode == "pull")
            {
                var url = s.Str("url");
                var slash = url.LastIndexOf('/');
                return "RTMP pull " + (slash > 0 ? url[..slash] + "/***" : url);
            }
            return $"RTMP push {s.Str("app")}/***";
        }
        return mode == "caller" ? $"SRT caller {s.Str("host")}:{s.Int("port")}" : $"SRT listener :{s.Int("port")}";
    }

    /// <summary>1 秒ごとの状態。イベントログに出す出来事を返す。</summary>
    public List<string> ApplyState(JsonNode st)
    {
        var events = new List<string>();
        var worker = st.At("worker");
        var video = st.At("video");
        var workerState = worker.Str("state", "stopped");
        var videoState = video.Str("state");
        var connected = video?.Bool("connected");
        Enabled = st.Bool("enabled");

        // こちらから接続しに行く方式 (RTMP pull / SRT caller) は「待ち受け」ではないので、言葉を分け、
        // 繋がらない理由 (worker の lastError) を出す
        var source = st.At("source");
        var outgoing = (source.Str("protocol"), source.Str("mode")) is ("rtmp", "pull") or ("srt", "caller");
        var lastError = video.Str("lastError");
        var unreachable = outgoing && connected != true && lastError.Length > 0;
        (StatusKind, StatusText) = workerState switch
        {
            "stopped" => ("stopped", "停止"),
            "unavailable" => ("restarting", "FFmpeg がありません"),
            "restarting" => ("restarting", "再起動中"),
            _ when video == null => ("starting", "起動中"),
            _ when videoState == "live" => ("live", "受信中"),
            _ when videoState == "hold" => ("hold", "途絶 (最後の映像を保持)"),
            _ when unreachable => ("unreachable", "接続できません"),
            _ when outgoing && connected == true => ("waiting", "映像を待っています"),
            _ when outgoing => ("waiting", "接続中…"),
            _ => ("waiting", "待ち受け中"),
        };
        UnreachableReason = unreachable ? lastError : "";

        // イベントログ
        var restarts = (long)worker.Num("restarts");
        if (_lastRestarts >= 0 && restarts > _lastRestarts) events.Add($"worker を再起動しました ({worker.Str("lastEvent")})");
        if (_lastWorkerState != "" && workerState != _lastWorkerState && (workerState == "stopped" || _lastWorkerState == "stopped"))
        {
            events.Add(workerState == "stopped" ? "停止しました" : "開始しました");
        }
        if (_lastConnected != null && connected != null && connected != _lastConnected)
        {
            events.Add(connected == true ? $"接続されました ({video.Str("peer")})" : "接続が切れました");
        }
        if (_lastVideoState != "" && videoState != _lastVideoState && videoState is "live" or "hold")
        {
            events.Add(videoState == "live" ? "映像を受信しています" : "映像が途絶えました (最後の映像を保持)");
        }
        // 繋がらない理由は変わったときだけ出す (繋ぎ直しのたびには出さない)
        if (UnreachableReason.Length > 0 && UnreachableReason != _lastUnreachableReason) events.Add($"接続できません: {UnreachableReason}");
        _lastUnreachableReason = UnreachableReason;
        _lastRestarts = restarts;
        _lastWorkerState = workerState;
        _lastConnected = connected;
        _lastVideoState = videoState;
        LastEvent = worker.Str("lastEvent");

        // 主要メトリクス
        var bitrate = video.Num("bitrateMbps");
        var delay = video.Num("delayMs");
        var srt = st.At("srt");
        var tcp = st.At("tcp");
        var rtt = srt != null ? srt.Num("rttMs") : tcp.Num("rttMs");
        if (video != null)
        {
            var res = video.Int("width") > 0 ? $"{video.Int("width")}x{video.Int("height")} " : "";
            VideoLine = $"{bitrate:0.0} Mbps   {video.Num("outputFps"):0} fps   遅延 {delay:0} ms   {res}{video.Str("codec")}";
            var recommended = srt.At("recommend").Int("latencyMs");
            TransportLine = srt != null
                ? $"SRT latency {srt.Int("latencyMs")} ms{(recommended > 0 ? $" (推奨 {recommended})" : "")}   RTT {rtt:0.0} ms   ロス {srt.Num("lost"):0}   ドロップ {srt.Num("dropped"):0}"
                : tcp != null ? $"TCP RTT {rtt:0.0} ms   再送 {tcp.Num("retransBytes") / 1024:0} KB" : "";
            // 受信中だけ出す (HW が使えれば何も出さない)
            SoftwareDecode = videoState is "live" or "hold" && !video.Bool("hwDecode");
            // 送出側との経路: 取りこぼし (直近 1 分) は赤、推奨 latency に足りない・再送があるなら黄
            var dropped = srt != null ? _srtDropped.Add(srt.Num("dropped")) : 0;
            var retrans = tcp != null ? _tcpRetrans.Add(tcp.Num("retransBytes")) : 0;
            TransportKind = dropped > 0 ? "error"
                            : (srt != null && recommended > srt.Int("latencyMs")) || retrans > 0 ? "warn"
                            : "ok";
        }
        else
        {
            VideoLine = "";
            TransportLine = "";
            SoftwareDecode = false;
            TransportKind = "ok";
            _srtDropped.Reset();
            _tcpRetrans.Reset();
        }
        if (unreachable)
        {
            // 繋がらないあいだは経路の数字 (0 ばかり) の代わりに理由を出す
            TransportLine = $"接続できません: {lastError}";
            TransportKind = "error";
        }
        SrtRecommendedLatencyMs = srt.At("recommend").Int("latencyMs");
        SrtRecommendation = srt != null ? DescribeSrtRecommendation(srt) : "";

        var audioIn = st.At("audioIn");
        var audioOut = st.At("audioOut");
        var peak = audioOut?.At("peakDb") as JsonArray ?? audioIn?.At("peakDb") as JsonArray;
        PeakL = peak is { Count: 2 } ? PeakOf(peak[0]) : -100;
        PeakR = peak is { Count: 2 } ? PeakOf(peak[1]) : -100;
        // 直近 1 分の音切れ (デバイスの音切れ + 合わせ直し、worker 側の抜け)
        var dropouts = audioOut != null && audioOut.At("latencyMs") != null
            ? _audioDropouts.Add(audioOut.Num("underruns") + audioOut.Num("resyncs"))
            : 0;
        var gaps = audioIn != null ? _audioGaps.Add(audioIn.Num("gaps")) : 0;
        var dropoutNote = dropouts + gaps > 0 ? $"  音切れ {dropouts + gaps:0} 回/分" : "";
        AudioLine = audioOut switch
        {
            null => "音声出力なし",
            _ when audioOut.At("error") != null => $"音声: {audioOut.Str("error")} ({audioOut.Str("device")})",
            _ when audioOut.Bool("playing") => $"音声 {audioOut.Num("latencyMs"):0} ms" + (Mute ? "  ミュート中" : "") + dropoutNote,
            _ => audioOut.Bool("open") ? "音声 待機中" : "音声デバイスを開けません",
        };
        AudioKind = audioOut == null ? "ok"
                    : audioOut.At("error") != null || (!audioOut.Bool("open") && !audioOut.Bool("playing")) ? "error"
                    : dropouts + gaps > 0 ? "warn"
                    : "ok";

        var record = st.At("record");
        var recState = record.Str("state", "off");
        if (recState != RecordKind)
        {
            if (recState == "recording") events.Add($"録画を開始しました ({Path.GetFileName(record.Str("file"))})");
            else if (RecordKind == "recording") events.Add("録画ファイルを閉じました");
            if (recState == "error") events.Add($"録画: {record.Str("error")}");
        }
        RecordKind = recState;
        RecordLine = recState switch
        {
            "recording" => $"● REC {FormatDuration(record.Num("seconds"))}  {FormatBytes(record.Num("bytes"))}  {Path.GetFileName(record.Str("file"))}",
            "waiting" => "● 録画待機 (映像を待っています)",
            "error" => $"録画エラー: {record.Str("error")}",
            _ => "",
        };

        Push(_bitrate, bitrate);
        Push(_delay, delay);
        Push(_rtt, rtt);
        BitrateHistory = [.. _bitrate];
        DelayHistory = [.. _delay];
        RttHistory = [.. _rtt];

        if (workerState == "stopped") PreviewHasFrame = PreviewHasFrame && Enabled;
        UpdatePreviewMessage();
        Details = BuildDetails(st);
        return events;
    }

    /// <summary>SRT の latency の推奨値 (受信中なら 0 より大きい。0 = 計測中 / 受信していない)</summary>
    public int SrtRecommendedLatencyMs { get; private set; }

    /// <summary>推奨値の説明 (設定画面にも出す)</summary>
    public string SrtRecommendation { get; private set; } = "";

    /// <summary>
    /// 受信状況から出した推奨値の説明。worker が直近 60 秒の RTT (最大) とロス率から出している
    /// (latency ≥ RTT × 倍率、倍率はロス率で 3〜10。取りこぼしがあれば今の 1.5 倍以上)。
    /// </summary>
    public static string DescribeSrtRecommendation(JsonNode srt)
    {
        var r = srt.At("recommend");
        var recommended = r.Int("latencyMs");
        if (recommended <= 0) return $"計測中 (受信 {r.Int("windowSec")} 秒。10 秒分の統計が必要)";
        var effective = srt.Int("latencyMs");
        var configured = srt.Int("configuredLatencyMs");
        var text = $"{recommended} ms (RTT {r.Num("rttMs"):0.0} ms × {r.Int("multiplier")}、ロス {r.Num("lossPct"):0.##}%、直近 {r.Int("windowSec")} 秒)";
        if (r.Num("drops") > 0)
        {
            return text + $"。取りこぼし {r.Num("drops"):0}: 今の {effective} ms では足りていません";
        }
        if (recommended < effective && effective > configured)
        {
            return text + $"。相手側の latency ({effective} ms) が使われています。下げるには相手側も下げてください";
        }
        if (recommended < effective) return text + $"。今の {effective} ms から下げられます";
        if (recommended > effective) return text + $"。今の {effective} ms では足りない可能性があります";
        return text;
    }

    /// <summary>経過時間を h:mm:ss で (TimeSpan の "hh" は 24 時間で 0 に戻るので使わない)</summary>
    private static double PeakOf(JsonNode? v) => v is JsonValue j && j.TryGetValue<double>(out var d) && double.IsFinite(d) ? d : -100;

    private static string FormatDuration(double seconds)
    {
        if (!double.IsFinite(seconds) || seconds < 0) seconds = 0;
        var t = TimeSpan.FromSeconds(seconds);
        return $"{(long)t.TotalHours}:{t.Minutes:00}:{t.Seconds:00}";
    }

    private static string FormatBytes(double b) =>
        b >= 1024.0 * 1024 * 1024 ? $"{b / (1024.0 * 1024 * 1024):0.00} GB" : $"{b / (1024.0 * 1024):0} MB";

    private static void Push(List<double> list, double v)
    {
        list.Add(v);
        if (list.Count > HistoryLength) list.RemoveAt(0);
    }

    private List<DetailRow> BuildDetails(JsonNode st)
    {
        var rows = new List<DetailRow>();
        void Add(string label, string value) => rows.Add(new DetailRow(label, value));
        var worker = st.At("worker");
        var video = st.At("video");
        Add("状態", StatusText);
        Add("入力", SourceSummary);
        if (video != null)
        {
            Add("接続元", video.Str("peer") is { Length: > 0 } p ? p : "-");
            Add("接続回数", $"{video.Num("connections"):0}");
            Add("映像", $"{video.Str("codec")} {video.Int("width")}x{video.Int("height")} ({(video.Bool("hwDecode") ? "HW" : "SW")} デコード)");
            Add("ビットレート", $"{video.Num("bitrateMbps"):0.00} Mbps");
            Add("デコード / 出力", $"{video.Num("decodeFps"):0} / {video.Num("outputFps"):0} fps");
            Add("映像の遅延", $"{video.Num("delayMs"):0} ms (デコード後から仮想カメラまで)");
            Add("落としたフレーム", $"遅れ {video.Num("droppedLate"):0} / 溢れ {video.Num("droppedQueue"):0}");
            Add("デコードエラー", $"{video.Num("decodeErrors"):0}");
            if (video.Str("lastError") is { Length: > 0 } err) Add(UnreachableReason.Length > 0 ? "接続できない理由" : "エラー", err);
        }
        if (st.At("srt") is JsonNode srt)
        {
            Add("SRT latency", $"{srt.Int("latencyMs")} ms (送出側との大きい方)");
            Add("SRT RTT", $"{srt.Num("rttMs"):0.0} ms");
            Add("SRT 受信バッファ", $"{srt.Int("rcvBufMs")} ms");
            Add("SRT ロス / 再送 / ドロップ", $"{srt.Num("lost"):0} / {srt.Num("retrans"):0} / {srt.Num("dropped"):0}");
            Add("SRT 推奨 latency", DescribeSrtRecommendation(srt));
        }
        if (st.At("tcp") is JsonNode tcp)
        {
            Add("TCP RTT", $"{tcp.Num("rttMs"):0.0} ms");
            Add("TCP 再送", $"{tcp.Num("retransBytes") / 1024:0} KB");
        }
        if (st.At("audioIn") is JsonNode ai)
        {
            Add("音声 (worker)", $"遅延 {ai.Num("delayMs"):0} ms / A/V {ai.Num("avOffsetMs"):+0;-0;0} ms / 抜け {ai.Num("gaps"):0}");
        }
        if (st.At("audioOut") is JsonNode ao)
        {
            Add("音声出力", $"{ao.Str("device")} {ao.Int("startChannel")}-{ao.Int("startChannel") + 1}{(ao.Bool("exclusive") ? " (排他)" : "")}");
            if (ao.At("latencyMs") != null)
            {
                Add("音声の遅延", $"{ao.Num("latencyMs"):0.0} ms (ずれ {ao.Num("errorMs"):+0.00;-0.00} ms)");
                Add("重複・間引き / 音切れ / 合わせ直し", $"{ao.Num("slips"):0} / {ao.Num("underruns"):0} / {ao.Num("resyncs"):0}");
            }
        }
        if (st.At("record") is JsonNode rec && rec.Str("state") != "off")
        {
            Add("録画", rec.Str("state") switch { "recording" => "録画中", "waiting" => "映像待ち", "error" => "エラー", _ => "-" });
            if (rec.Str("file") is { Length: > 0 } file) Add("録画ファイル", file);
            Add("録画サイズ / 捨てた数", $"{FormatBytes(rec.Num("bytes"))} / {rec.Num("dropped"):0}");
            if (rec.Str("error") is { Length: > 0 } recErr) Add("録画エラー", recErr);
        }
        Add("worker", $"{worker.Str("state")} pid {worker.Num("pid"):0}  稼働 {FormatDuration(worker.Num("uptimeSec"))}  再起動 {worker.Num("restarts"):0} 回");
        if (worker.Str("lastEvent") is { Length: > 0 } ev) Add("直近の異常", ev);
        return rows;
    }

    public void Dispose() => _preview.Dispose();
}
