using System.Collections.ObjectModel;
using System.IO;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Windows;
using System.Windows.Input;
using System.Windows.Threading;
using Denpachan.Ui.Engine;
using Denpachan.Ui.Infrastructure;

namespace Denpachan.Ui.ViewModels;

public sealed record AudioDeviceItem(string Name, int Channels, int SampleRate);

public sealed record EventItem(DateTime Time, string Text, bool Warning)
{
    public string TimeText => Time.ToString("HH:mm:ss");
}

/// <summary>画面全体</summary>
public sealed class MainViewModel : ObservableObject, IDisposable
{
    private readonly EngineClient _client = new();
    private readonly Dispatcher _dispatcher = Application.Current.Dispatcher;
    private readonly DispatcherTimer _previewTimer;
    private readonly DispatcherTimer _systemTimer;
    private readonly SystemStats _systemStats = new();
    private bool _systemSampling;
    private long _lastRtmpRestarts = -1;

    public MainViewModel()
    {
        for (var i = 1; i <= 4; i++) Channels.Add(new ChannelViewModel(this, i));
        LoadUiSettings();
        Selected = Channels[0];

        _client.ConnectionChanged += c => _dispatcher.InvokeAsync(() => OnConnectionChanged(c));
        _client.StateReceived += s => _dispatcher.InvokeAsync(() => OnState(s));

        _previewTimer = new DispatcherTimer(TimeSpan.FromMilliseconds(66), DispatcherPriority.Render, (_, _) =>
        {
            foreach (var ch in Channels) ch.PollPreview();
        }, _dispatcher);

        // PC の様子 (CPU、メモリ、録画先の空き) は 2 秒ごと。読むだけなので軽いが、録画先がネットワーク
        // ドライブだと待たされることがあるので UI スレッドでは読まない
        _systemTimer = new DispatcherTimer(TimeSpan.FromSeconds(2), DispatcherPriority.Background, (_, _) => _ = SampleSystemAsync(), _dispatcher);

        RefreshDevicesCommand = new RelayCommand(() => _ = LoadDevicesAsync(), () => Connected);
        GeneralSettingsCommand = new RelayCommand(OpenGeneralSettings, () => Connected && GeneralConfig != null);
        ShutdownCommand = new RelayCommand(() => Application.Current.MainWindow?.Close());  // 閉じるときの確認を通る
        SelectCommand = new RelayCommand(p => { if (p is ChannelViewModel ch) Selected = ch; });
        UnregisterVcamCommand = new RelayCommand(UnregisterVcam);
    }

    public void Start()
    {
        if (!FixtureMode)
        {
            EnsureVcamRegistered();
            _client.Start();
        }
        _previewTimer.Start();
    }

    // ---- 試験用 (UiFixture): engine に繋がずに設定と状態を流し込む ----

    /// <summary>試験用の状態を流し込んでいる (engine に繋がず、画面の設定も保存しない)</summary>
    public bool FixtureMode { get; set; }

    internal void SetConnectedFromFixture(bool connected) => Connected = connected;
    internal void SetGeneralConfigFromFixture(JsonNode general) => GeneralConfig = general;
    internal void ApplyStateFromFixture(JsonNode state) => OnState(state);

    // ---- 仮想カメラの登録 (zip を展開してすぐ使えるように、このユーザーに自動で登録する) ----

    private void EnsureVcamRegistered()
    {
        if (VcamRegistration.IsRegistered()) return;
        var error = VcamRegistration.RegisterForUser();
        if (error == null && VcamRegistration.IsRegistered())
        {
            Log("仮想カメラ (denpachan Cam 1〜4) をこのユーザーに登録しました。OBS では「映像キャプチャデバイス」から選べます");
        }
        else
        {
            Log($"仮想カメラを登録できませんでした: {error ?? "登録を確認できません"}", true);
        }
    }

    private void UnregisterVcam()
    {
        var answer = MessageBox.Show(Application.Current.MainWindow,
            "このユーザーの仮想カメラ (denpachan Cam 1〜4) の登録を解除します。\n" +
            "denpachan のフォルダを消す前に使います。次に denpachan を起動すると登録し直されます。\n" +
            "OBS などで使っている間は、解除すると映らなくなります。よろしいですか?",
            "denpachan", MessageBoxButton.OKCancel, MessageBoxImage.Question);
        if (answer != MessageBoxResult.OK) return;
        var error = VcamRegistration.UnregisterForUser();
        Log(error == null ? "仮想カメラの登録を解除しました (このユーザー)" : $"仮想カメラの登録を解除できませんでした: {error}", error != null);
    }

    public ObservableCollection<ChannelViewModel> Channels { get; } = [];
    public ObservableCollection<EventItem> Events { get; } = [];
    public List<AudioDeviceItem> AudioDevices { get; private set; } = [];
    public JsonNode? GeneralConfig { get; private set; }

    public ICommand RefreshDevicesCommand { get; }
    public ICommand GeneralSettingsCommand { get; }
    public ICommand ShutdownCommand { get; }
    public ICommand SelectCommand { get; }
    public ICommand UnregisterVcamCommand { get; }

    private ChannelViewModel? _selected;
    public ChannelViewModel? Selected
    {
        get => _selected;
        set
        {
            if (_selected != null) _selected.IsSelected = false;
            SetProperty(ref _selected, value);
            if (_selected != null) _selected.IsSelected = true;
        }
    }

    private bool _connected;
    public bool Connected { get => _connected; private set { if (SetProperty(ref _connected, value)) OnPropertyChanged(nameof(ConnectionText)); } }
    public string ConnectionText => Connected ? "engine: 接続中" : "engine: 接続していません (起動を待っています)";

    private string _rtmpText = "";
    public string RtmpText { get => _rtmpText; private set => SetProperty(ref _rtmpText, value); }

    private string _systemText = "";
    /// <summary>CPU 使用率、メモリの空き、録画先ドライブの空き</summary>
    public string SystemText { get => _systemText; private set => SetProperty(ref _systemText, value); }

    private string _systemKind = "ok";
    /// <summary>ok / warn / error (CPU が高い、メモリや録画先の空きが少ない)</summary>
    public string SystemKind { get => _systemKind; private set => SetProperty(ref _systemKind, value); }

    private async Task SampleSystemAsync()
    {
        if (_systemSampling) return;  // 前回がまだ終わっていない (ネットワークドライブ待ち)
        _systemSampling = true;
        try
        {
            // 録画先: 録画がオンのチャンネルのフォルダ (空 = Videos\denpachan)。無ければシステムドライブ
            var drives = Channels.Where(c => c.Config != null && c.Config.At("record").Bool("enabled"))
                .Select(c => (Dir: c.Config!.At("record").Str("dir") is { Length: > 0 } d ? d : DefaultRecordDir,
                              MinFreeGb: (double)c.Config!.At("record").Int("minFreeGb", 5)))
                .ToList();
            if (drives.Count == 0) drives.Add((Environment.GetFolderPath(Environment.SpecialFolder.Windows), 5.0));
            var s = await Task.Run(() => _systemStats.Sample(drives));
            var kind = "ok";
            void Raise(string k) { if (k == "error" || (k == "warn" && kind == "ok")) kind = k; }
            if (s.CpuPercent >= 90) Raise("warn");
            if (s.MemoryFreeGb < 1) Raise("error"); else if (s.MemoryFreeGb < 2) Raise("warn");
            var disks = new List<string>();
            foreach (var d in s.Drives)
            {
                if (d.FreeGb < d.MinFreeGb) Raise("error"); else if (d.FreeGb < d.MinFreeGb * 2) Raise("warn");
                disks.Add($"{d.Root.TrimEnd('\\')} {FormatGb(d.FreeGb)}");
            }
            SystemText = $"CPU {s.CpuPercent:0}%   メモリ 空き {FormatGb(s.MemoryFreeGb)}" + (disks.Count > 0 ? $"   ディスク {string.Join(", ", disks)}" : "");
            SystemKind = kind;
        }
        catch
        {
            SystemText = "";
        }
        finally
        {
            _systemSampling = false;
        }
    }

    private static string DefaultRecordDir =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Videos", "denpachan");

    private static string FormatGb(double gb) => gb >= 1000 ? $"{gb / 1024:0.00} TB" : $"{gb:0} GB";

    private bool _ffmpegMissing;
    public bool FfmpegMissing { get => _ffmpegMissing; private set => SetProperty(ref _ffmpegMissing, value); }

    private string _ffmpegMessage = "";
    public string FfmpegMessage { get => _ffmpegMessage; private set => SetProperty(ref _ffmpegMessage, value); }

    // ---- engine とのやりとり ----

    private void OnConnectionChanged(bool connected)
    {
        Connected = connected;
        Log(connected ? "engine に接続しました" : "engine との接続が切れました", !connected);
        CommandManager.InvalidateRequerySuggested();
        if (connected)
        {
            _ = LoadDevicesAsync();
            _ = LoadConfigAsync();
        }
    }

    private void OnState(JsonNode state)
    {
        if (state.At("channels") is JsonArray channels)
        {
            foreach (var st in channels)
            {
                var id = st.Int("id");
                if (id < 1 || id > Channels.Count) continue;
                var ch = Channels[id - 1];
                foreach (var e in ch.ApplyState(st!)) Log($"{ch.Name}: {e}", e.Contains("切れ") || e.Contains("途絶") || e.Contains("再起動"));
            }
        }
        var ffmpeg = state.At("ffmpeg");
        var missing = ffmpeg != null && !ffmpeg.Bool("ok", true);
        if (missing != FfmpegMissing)
        {
            Log(missing ? ffmpeg.Str("message") : "FFmpeg が見つかりました", missing);
            FfmpegMissing = missing;
        }
        FfmpegMessage = missing ? ffmpeg.Str("message") + " (見つかると自動で受信を始めます)" : "";
        var rtmp = state.At("rtmpServer");
        var rtmpState = rtmp.Str("state", "stopped");
        RtmpText = rtmpState == "stopped" ? "RTMP サーバ: 停止" : $"RTMP サーバ: {(rtmpState == "running" ? "動作中" : "再起動中")} (:{rtmp.Int("port")})";
        var restarts = (long)rtmp.Num("restarts");
        if (_lastRtmpRestarts >= 0 && restarts > _lastRtmpRestarts) Log($"RTMP サーバを再起動しました ({rtmp.Str("lastEvent")})", true);
        _lastRtmpRestarts = restarts;
    }

    public async Task LoadConfigAsync()
    {
        try
        {
            var config = await _client.CallAsync("getConfig");
            GeneralConfig = config.At("general")?.DeepClone();
            if (config.At("channels") is JsonArray arr)
            {
                foreach (var c in arr)
                {
                    var id = c.Int("id");
                    if (id >= 1 && id <= Channels.Count) Channels[id - 1].ApplyConfig(c!);
                }
            }
            CommandManager.InvalidateRequerySuggested();
        }
        catch (EngineException e)
        {
            Log($"設定を読めませんでした: {e.Message}", true);
        }
    }

    public async Task LoadDevicesAsync()
    {
        try
        {
            var result = await _client.CallAsync("listAudioDevices");
            AudioDevices = result is JsonArray arr
                ? arr.Select(d => new AudioDeviceItem(d.Str("name"), d.Int("channels"), d.Int("sampleRate"))).ToList()
                : [];
            foreach (var ch in Channels) ch.RefreshAudioDeviceChoices();
        }
        catch (EngineException e)
        {
            Log($"音声デバイスを取得できませんでした: {e.Message}", true);
        }
    }

    /// <summary>要求を送る。失敗したらログに出して false。</summary>
    public async Task<bool> CallAsync(string method, JsonObject? parameters = null)
    {
        try
        {
            await _client.CallAsync(method, parameters);
            if (method is "startChannel" or "stopChannel" or "setMute" or "setRecording") await LoadConfigAsync();
            return true;
        }
        catch (EngineException e)
        {
            Log($"操作できませんでした: {e.Message}", true);
            return false;
        }
    }

    /// <summary>チャンネル設定の部分変更。成功したら画面に反映する。エラーはメッセージを返す。</summary>
    public async Task<string?> TryPatchChannelAsync(int ch, JsonObject patch)
    {
        try
        {
            var result = await _client.CallAsync("patchChannel", new JsonObject { ["ch"] = ch, ["patch"] = patch });
            if (result != null) Channels[ch - 1].ApplyConfig(result);
            return null;
        }
        catch (EngineException e)
        {
            return e.Message;
        }
    }

    public async Task<bool> PatchChannelAsync(int ch, JsonObject patch)
    {
        var error = await TryPatchChannelAsync(ch, patch);
        if (error == null) return true;
        Log($"設定を変更できませんでした: {error}", true);
        MessageBox.Show(Application.Current.MainWindow, error, "denpachan", MessageBoxButton.OK, MessageBoxImage.Warning);
        return false;
    }

    public async Task<string?> TryPatchGeneralAsync(JsonObject patch)
    {
        try
        {
            GeneralConfig = await _client.CallAsync("patchGeneral", new JsonObject { ["patch"] = patch });
            return null;
        }
        catch (EngineException e)
        {
            return e.Message;
        }
    }

    public void OpenChannelSettings(ChannelViewModel ch)
    {
        if (ch.Config == null) return;
        var window = new Views.ChannelSettingsWindow(this, ch) { Owner = Application.Current.MainWindow };
        window.ShowDialog();
    }

    private void OpenGeneralSettings()
    {
        if (GeneralConfig == null) return;
        var window = new Views.GeneralSettingsWindow(this, GeneralConfig) { Owner = Application.Current.MainWindow };
        window.ShowDialog();
    }

    // ---- 終了 (画面を閉じると受信も止める) ----

    /// <summary>
    /// 閉じてよいか確かめる。受信を開始しているチャンネルがあれば、止めてよいかを聞く (既定はキャンセル)。
    /// false = 閉じない。
    /// </summary>
    public bool ConfirmClose()
    {
        if (FixtureMode) return true;
        var active = Channels.Where(c => c.Enabled).ToList();
        if (active.Count == 0) return true;
        var names = string.Join("、", active.Select(c => c.Name));
        var recording = active.Any(c => c.RecordKind == "recording") ? "\n録画中のファイルも閉じます。" : "";
        var answer = MessageBox.Show(Application.Current.MainWindow,
            $"受信中のチャンネルがあります ({names})。\n" +
            $"閉じると受信を止め、仮想カメラと音声の出力も止まります。{recording}\n閉じてよいですか?",
            "denpachan", MessageBoxButton.OKCancel, MessageBoxImage.Warning, MessageBoxResult.Cancel);
        return answer == MessageBoxResult.OK;
    }

    /// <summary>engine を worker ごと止める。繋がっていなければ何もしない</summary>
    public async Task StopEngineAsync()
    {
        if (!Connected) return;
        try
        {
            await _client.CallAsync("shutdown").WaitAsync(TimeSpan.FromSeconds(3));
        }
        catch (Exception)
        {
            // 応答がなくても、送れなくても閉じる (engine は要求を受けていれば自分で止まる)
        }
    }

    // ---- イベントログ ----

    public void Log(string text, bool warning = false)
    {
        Events.Insert(0, new EventItem(DateTime.Now, text, warning));
        while (Events.Count > 500) Events.RemoveAt(Events.Count - 1);
    }

    // ---- 画面の設定 (プレビューのオン・オフ、操作ロック) ----

    private static string UiSettingsPath =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "denpachan", "ui.json");

    private bool _loadingUiSettings;

    private void LoadUiSettings()
    {
        try
        {
            if (!File.Exists(UiSettingsPath)) return;
            var json = JsonNode.Parse(File.ReadAllText(UiSettingsPath));
            _loadingUiSettings = true;
            if (json.At("preview") is JsonArray preview)
            {
                for (var i = 0; i < Math.Min(preview.Count, Channels.Count); i++) Channels[i].PreviewEnabled = preview[i]?.GetValue<bool>() ?? true;
            }
            if (json.At("locked") is JsonArray locked)
            {
                for (var i = 0; i < Math.Min(locked.Count, Channels.Count); i++) Channels[i].IsLocked = locked[i]?.GetValue<bool>() ?? false;
            }
        }
        catch
        {
            // 壊れていたら既定値のまま
        }
        finally
        {
            _loadingUiSettings = false;
        }
    }

    public void SaveUiSettings()
    {
        if (_loadingUiSettings || FixtureMode) return;
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(UiSettingsPath)!);
            var json = new JsonObject
            {
                ["preview"] = new JsonArray(Channels.Select(c => (JsonNode)c.PreviewEnabled).ToArray()),
                ["locked"] = new JsonArray(Channels.Select(c => (JsonNode)c.IsLocked).ToArray()),
            };
            File.WriteAllText(UiSettingsPath, json.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
        }
        catch
        {
            // 保存できなくても動作には影響しない
        }
    }

    public void Dispose()
    {
        _previewTimer.Stop();
        _systemTimer.Stop();
        _client.Dispose();
        foreach (var ch in Channels) ch.Dispose();
    }
}
