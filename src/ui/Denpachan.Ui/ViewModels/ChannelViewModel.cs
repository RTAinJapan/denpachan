using System.IO;
using System.Text.Json.Nodes;
using System.Windows.Input;
using System.Windows.Media.Imaging;
using Denpachan.Ui.Engine;
using Denpachan.Ui.Infrastructure;
using static Denpachan.Ui.Infrastructure.Loc;

namespace Denpachan.Ui.ViewModels;

public sealed record Option<T>(T Value, string Label);

public sealed record DetailRow(string Label, string Value);

/// <summary>イベントログに出す出来事 (Warning は黄色で出す)</summary>
public sealed record ChannelEvent(string Text, bool Warning = false);

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
    private string _lastUnreachable = "";  // 繋がらない理由 (メッセージの JSON。言語を変えても同じ)
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
    public string Name { get => _name; private set { if (SetProperty(ref _name, value)) OnPropertyChanged(nameof(DetailTitle)); } }

    /// <summary>詳細パネルの見出し</summary>
    public string DetailTitle => F("main.detail.title", ("name", Name));

    private bool _enabled;
    public bool Enabled { get => _enabled; private set { if (SetProperty(ref _enabled, value)) OnPropertyChanged(nameof(StartStopLabel)); } }
    public string StartStopLabel => T(Enabled ? "main.tile.stop" : "main.tile.start");

    private string _statusKind = "stopped";
    /// <summary>stopped / starting / waiting / unreachable / live / hold / restarting</summary>
    public string StatusKind { get => _statusKind; private set => SetProperty(ref _statusKind, value); }

    private string _statusText = T("status.stopped");
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
        PreviewMessage = !PreviewEnabled ? T("main.preview.off") : !Enabled ? T("main.preview.stopped")
                         : PreviewHasFrame ? "" : T("main.preview.noSignal");
    }

    // ---- ルーティング (プルダウン) ----

    public IReadOnlyList<Option<int>> VcamOptions =>
        [new(0, T("common.none")), new(1, "Cam 1"), new(2, "Cam 2"), new(3, "Cam 3"), new(4, "Cam 4")];

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

    /// <summary>音声の出力先 (Value は設定に書くデバイス名、"" = なし)</summary>
    private IReadOnlyList<Option<string>> _audioDeviceChoices = [];
    public IReadOnlyList<Option<string>> AudioDeviceChoices { get => _audioDeviceChoices; private set => SetProperty(ref _audioDeviceChoices, value); }

    private string _audioDevice = "";
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
            _ = PatchOrRevertAsync(new JsonObject { ["audio"] = new JsonObject { ["device"] = value } }, () => AudioDevice = old);
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
    public string MuteLabel => T(Mute ? "main.tile.muted" : "main.tile.mute");

    public void RefreshAudioDeviceChoices()
    {
        var choices = new List<Option<string>> { new("", T("common.none")) };
        choices.AddRange(_main.AudioDevices.Select(d => new Option<string>(d.Name, d.Name)));
        if (_audioDevice != "" && !choices.Any(c => c.Value == _audioDevice)) choices.Add(new(_audioDevice, _audioDevice));  // 設定にあるが見つからないデバイス
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

    // ---- 言語を変えたとき ----

    /// <summary>C# で組み立てている文言を作り直す (MainViewModel が最後の状態を流し直す)</summary>
    public void OnLanguageChanged()
    {
        OnPropertyChanged(nameof(StartStopLabel));
        OnPropertyChanged(nameof(MuteLabel));
        OnPropertyChanged(nameof(DetailTitle));
        var vcam = _vcam;
        _applying = true;
        OnPropertyChanged(nameof(VcamOptions));
        Vcam = vcam;
        _applying = false;
        RefreshAudioDeviceChoices();
        UpdatePreviewMessage();
    }

    // ---- engine から届いた設定・状態の反映 ----

    public void ApplyConfig(JsonNode config)
    {
        Config = config.DeepClone();
        _applying = true;
        Name = config.Str("name", $"CH{Id}");
        Enabled = config.Bool("enabled");
        Vcam = config.Int("vcam");
        var audio = config.At("audio");
        _audioDevice = audio.Str("device");
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

    /// <summary>
    /// 1 秒ごとの状態。イベントログに出す出来事を返す。
    /// relabel = 言語を変えたので文言だけ作り直す (グラフの履歴に足さない、出来事も返さない)。
    /// </summary>
    public List<ChannelEvent> ApplyState(JsonNode st, bool relabel = false)
    {
        var events = new List<ChannelEvent>();
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
        var lastError = Message(video.At("lastError"));
        var unreachable = outgoing && connected != true && lastError.Length > 0;
        (StatusKind, StatusText) = workerState switch
        {
            "stopped" => ("stopped", T("status.stopped")),
            "unavailable" => ("restarting", T("status.ffmpegMissing")),
            "restarting" => ("restarting", T("status.restarting")),
            _ when video == null => ("starting", T("status.starting")),
            _ when videoState == "live" => ("live", T("status.live")),
            _ when videoState == "hold" => ("hold", T("status.hold")),
            _ when unreachable => ("unreachable", T("status.unreachable")),
            _ when outgoing && connected == true => ("waiting", T("status.waitingVideo")),
            _ when outgoing => ("waiting", T("status.connecting")),
            _ => ("waiting", T("status.listening")),
        };
        UnreachableReason = unreachable ? lastError : "";

        // イベントログ
        var lastEvent = Message(worker.At("lastEvent"));
        var restarts = (long)worker.Num("restarts");
        if (_lastRestarts >= 0 && restarts > _lastRestarts) events.Add(new(F("event.workerRestarted", ("reason", lastEvent)), true));
        if (_lastWorkerState != "" && workerState != _lastWorkerState && (workerState == "stopped" || _lastWorkerState == "stopped"))
        {
            events.Add(new(T(workerState == "stopped" ? "event.stopped" : "event.started")));
        }
        if (_lastConnected != null && connected != null && connected != _lastConnected)
        {
            events.Add(connected == true ? new(F("event.connected", ("peer", video.Str("peer")))) : new(T("event.disconnected"), true));
        }
        if (_lastVideoState != "" && videoState != _lastVideoState && videoState is "live" or "hold")
        {
            events.Add(videoState == "live" ? new(T("event.live")) : new(T("event.hold"), true));
        }
        // 繋がらない理由は変わったときだけ出す (繋ぎ直しのたびには出さない)
        var unreachableKey = unreachable ? video.At("lastError")?.ToJsonString() ?? "" : "";
        if (unreachableKey.Length > 0 && unreachableKey != _lastUnreachable) events.Add(new(F("event.unreachable", ("reason", lastError)), true));
        _lastUnreachable = unreachableKey;
        _lastRestarts = restarts;
        _lastWorkerState = workerState;
        _lastConnected = connected;
        _lastVideoState = videoState;
        LastEvent = lastEvent;

        // 主要メトリクス
        var bitrate = video.Num("bitrateMbps");
        var delay = video.Num("delayMs");
        var srt = st.At("srt");
        var tcp = st.At("tcp");
        var rtt = srt != null ? srt.Num("rttMs") : tcp.Num("rttMs");
        if (video != null)
        {
            var res = video.Int("width") > 0 ? $"{video.Int("width")}x{video.Int("height")} " : "";
            VideoLine = F("main.tile.videoLine", ("bitrate", $"{bitrate:0.0}"), ("fps", $"{video.Num("outputFps"):0}"),
                          ("delay", $"{delay:0}"), ("res", res), ("codec", video.Str("codec")));
            var recommended = srt.At("recommend").Int("latencyMs");
            TransportLine = srt != null
                ? F(recommended > 0 ? "main.tile.srtLineWithRecommend" : "main.tile.srtLine", ("latency", srt.Int("latencyMs")),
                    ("recommend", recommended), ("rtt", $"{rtt:0.0}"), ("lost", $"{srt.Num("lost"):0}"), ("dropped", $"{srt.Num("dropped"):0}"))
                : tcp != null ? F("main.tile.tcpLine", ("rtt", $"{rtt:0.0}"), ("retrans", $"{tcp.Num("retransBytes") / 1024:0}")) : "";
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
            TransportLine = F("main.tile.unreachable", ("reason", lastError));
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
        // 再生中の補足 (ミュート、音切れ)。あれば「、」でつないで後ろに出す
        var audioNotes = new List<string>();
        if (Mute) audioNotes.Add(T("main.tile.audioMuted"));
        if (dropouts + gaps > 0) audioNotes.Add(F("main.tile.audioDropouts", ("count", $"{dropouts + gaps:0}")));
        var audioLatency = $"{audioOut.Num("latencyMs"):0}";
        AudioLine = audioOut switch
        {
            null => T("main.tile.audioNone"),
            _ when audioOut.At("error") != null => F("main.tile.audioError", ("error", Message(audioOut.At("error"))), ("device", audioOut.Str("device"))),
            _ when audioOut.Bool("playing") => audioNotes.Count == 0
                ? F("main.tile.audioPlaying", ("latency", audioLatency))
                : F("main.tile.audioPlayingWithNotes", ("latency", audioLatency), ("notes", string.Join(T("common.listSeparator"), audioNotes))),
            _ => T(audioOut.Bool("open") ? "main.tile.audioIdle" : "main.tile.audioCannotOpen"),
        };
        AudioKind = audioOut == null ? "ok"
                    : audioOut.At("error") != null || (!audioOut.Bool("open") && !audioOut.Bool("playing")) ? "error"
                    : dropouts + gaps > 0 ? "warn"
                    : "ok";

        var record = st.At("record");
        var recState = record.Str("state", "off");
        var recError = Message(record.At("error"));
        if (recState != RecordKind)
        {
            if (recState == "recording") events.Add(new(F("event.recStarted", ("file", Path.GetFileName(record.Str("file"))))));
            else if (RecordKind == "recording") events.Add(new(T("event.recClosed")));
            if (recState == "error") events.Add(new(F("event.recError", ("error", recError)), true));
        }
        RecordKind = recState;
        RecordLine = recState switch
        {
            "recording" => F("main.tile.recRecording", ("duration", FormatDuration(record.Num("seconds"))),
                             ("size", FormatBytes(record.Num("bytes"))), ("file", Path.GetFileName(record.Str("file")))),
            "waiting" => T("main.tile.recWaiting"),
            "error" => F("main.tile.recError", ("error", recError)),
            _ => "",
        };

        if (!relabel)
        {
            Push(_bitrate, bitrate);
            Push(_delay, delay);
            Push(_rtt, rtt);
            BitrateHistory = [.. _bitrate];
            DelayHistory = [.. _delay];
            RttHistory = [.. _rtt];
        }

        if (workerState == "stopped") PreviewHasFrame = PreviewHasFrame && Enabled;
        UpdatePreviewMessage();
        Details = BuildDetails(st);
        return relabel ? [] : events;
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
        if (recommended <= 0) return F("srtAdvice.measuring", ("sec", r.Int("windowSec")));
        var effective = srt.Int("latencyMs");
        var configured = srt.Int("configuredLatencyMs");
        var text = F("srtAdvice.value", ("latency", recommended), ("rtt", $"{r.Num("rttMs"):0.0}"), ("multiplier", r.Int("multiplier")),
                     ("loss", $"{r.Num("lossPct"):0.##}"), ("sec", r.Int("windowSec")));
        var note = r.Num("drops") > 0 ? F("srtAdvice.drops", ("drops", $"{r.Num("drops"):0}"), ("current", effective))
                   : recommended < effective && effective > configured ? F("srtAdvice.peerLatency", ("current", effective))
                   : recommended < effective ? F("srtAdvice.canLower", ("current", effective))
                   : recommended > effective ? F("srtAdvice.notEnough", ("current", effective))
                   : "";
        return note.Length > 0 ? F("srtAdvice.withNote", ("value", text), ("note", note)) : text;
    }

    private static double PeakOf(JsonNode? v) => v is JsonValue j && j.TryGetValue<double>(out var d) && double.IsFinite(d) ? d : -100;

    /// <summary>経過時間を h:mm:ss で (TimeSpan の "hh" は 24 時間で 0 に戻るので使わない)</summary>
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
        void Add(string labelKey, string value) => rows.Add(new DetailRow(T(labelKey), value));
        var worker = st.At("worker");
        var video = st.At("video");
        Add("detail.status", StatusText);
        Add("detail.input", SourceSummary);
        if (video != null)
        {
            Add("detail.peer", video.Str("peer") is { Length: > 0 } p ? p : "-");
            Add("detail.connections", $"{video.Num("connections"):0}");
            Add("detail.video", F(video.Bool("hwDecode") ? "detail.videoValueHw" : "detail.videoValueSw", ("codec", video.Str("codec")),
                                  ("width", video.Int("width")), ("height", video.Int("height"))));
            Add("detail.bitrate", $"{video.Num("bitrateMbps"):0.00} Mbps");
            Add("detail.fps", $"{video.Num("decodeFps"):0} / {video.Num("outputFps"):0} fps");
            Add("detail.delay", F("detail.delayValue", ("ms", $"{video.Num("delayMs"):0}")));
            Add("detail.dropped", F("detail.droppedValue", ("late", $"{video.Num("droppedLate"):0}"), ("queue", $"{video.Num("droppedQueue"):0}")));
            Add("detail.decodeErrors", $"{video.Num("decodeErrors"):0}");
            if (Message(video.At("lastError")) is { Length: > 0 } err) Add(UnreachableReason.Length > 0 ? "detail.unreachableReason" : "detail.error", err);
        }
        if (st.At("srt") is JsonNode srt)
        {
            Add("detail.srtLatency", $"{srt.Int("latencyMs")} ms");
            Add("detail.srtRtt", $"{srt.Num("rttMs"):0.0} ms");
            Add("detail.srtBuffer", $"{srt.Int("rcvBufMs")} ms");
            Add("detail.srtLoss", $"{srt.Num("lost"):0} / {srt.Num("retrans"):0} / {srt.Num("dropped"):0}");
            Add("detail.srtRecommend", DescribeSrtRecommendation(srt));
        }
        if (st.At("tcp") is JsonNode tcp)
        {
            Add("detail.tcpRtt", $"{tcp.Num("rttMs"):0.0} ms");
            Add("detail.tcpRetrans", $"{tcp.Num("retransBytes") / 1024:0} KB");
        }
        if (st.At("audioIn") is JsonNode ai)
        {
            Add("detail.audioIn", F("detail.audioInValue", ("delay", $"{ai.Num("delayMs"):0}"), ("av", $"{ai.Num("avOffsetMs"):+0;-0;0}"),
                                    ("gaps", $"{ai.Num("gaps"):0}")));
        }
        if (st.At("audioOut") is JsonNode ao)
        {
            Add("detail.audioOut", F(ao.Bool("exclusive") ? "detail.audioOutValueExclusive" : "detail.audioOutValue",
                                     ("device", ao.Str("device")), ("channels", $"{ao.Int("startChannel")}-{ao.Int("startChannel") + 1}")));
            if (ao.At("latencyMs") != null)
            {
                Add("detail.audioLatency", F("detail.audioLatencyValue", ("latency", $"{ao.Num("latencyMs"):0.0}"),
                                             ("error", $"{ao.Num("errorMs"):+0.00;-0.00}")));
                Add("detail.audioSlips", $"{ao.Num("slips"):0} / {ao.Num("underruns"):0} / {ao.Num("resyncs"):0}");
            }
        }
        if (st.At("record") is JsonNode rec && rec.Str("state") != "off")
        {
            Add("detail.record", rec.Str("state") switch
            {
                "recording" => T("detail.recordRecording"),
                "waiting" => T("detail.recordWaiting"),
                "error" => T("detail.recordError"),
                _ => "-",
            });
            if (rec.Str("file") is { Length: > 0 } file) Add("detail.recordFile", file);
            Add("detail.recordSize", $"{FormatBytes(rec.Num("bytes"))} / {rec.Num("dropped"):0}");
            if (Message(rec.At("error")) is { Length: > 0 } recErr) Add("detail.recordErrorMessage", recErr);
        }
        Add("detail.worker", F("detail.workerValue", ("state", worker.Str("state")), ("pid", $"{worker.Num("pid"):0}"),
                               ("uptime", FormatDuration(worker.Num("uptimeSec"))), ("restarts", $"{worker.Num("restarts"):0}")));
        if (LastEvent.Length > 0) Add("detail.lastEvent", LastEvent);
        return rows;
    }

    public void Dispose() => _preview.Dispose();
}
