using System.Collections.ObjectModel;
using System.IO;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Windows;
using System.Windows.Input;
using System.Windows.Threading;
using Denpachan.Ui.Engine;
using Denpachan.Ui.Infrastructure;
using static Denpachan.Ui.Infrastructure.Loc;

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
        // チャンネルは上限の数だけ作っておき、画面には使う数 (全体設定の channelCount) だけ出す
        for (var i = 1; i <= VcamRegistration.Count; i++) Channels.Add(new ChannelViewModel(this, i));
        LoadUiSettings();
        UpdateVisibleChannels();

        _client.ConnectionChanged += c => _dispatcher.InvokeAsync(() => OnConnectionChanged(c));
        _client.StateReceived += s => _dispatcher.InvokeAsync(() => OnState(s));

        _previewTimer = new DispatcherTimer(TimeSpan.FromMilliseconds(66), DispatcherPriority.Render, (_, _) =>
        {
            // 最小化している間は読まない (見えないものを描かない。読まなければ worker も 2 秒で縮小をやめる)
            if (Application.Current?.MainWindow?.WindowState == WindowState.Minimized) return;
            foreach (var ch in VisibleChannels) ch.PollPreview();
        }, _dispatcher);

        // PC の様子 (CPU、メモリ、録画先の空き) は 2 秒ごと。読むだけなので軽いが、録画先がネットワーク
        // ドライブだと待たされることがあるので UI スレッドでは読まない
        _systemTimer = new DispatcherTimer(TimeSpan.FromSeconds(2), DispatcherPriority.Background, (_, _) => _ = SampleSystemAsync(), _dispatcher);

        RefreshDevicesCommand = new RelayCommand(() => _ = LoadDevicesAsync(), () => Connected);
        GeneralSettingsCommand = new RelayCommand(OpenGeneralSettings, () => Connected && GeneralConfig != null);
        ShutdownCommand = new RelayCommand(() => Application.Current.MainWindow?.Close());  // 閉じるときの確認を通る
        SelectCommand = new RelayCommand(p => { if (p is ChannelViewModel ch) Selected = ch; });
        UnregisterVcamCommand = new RelayCommand(UnregisterVcam);
        OpenSettingsFolderCommand = new RelayCommand(OpenSettingsFolder);
        Loc.Instance.LanguageChanged += OnLanguageChanged;
    }

    // ---- 言語 (全体設定で変える。画面の設定 ui.json に保存する) ----

    /// <summary>言語の設定 ("auto" か言語コード)。変えるとその場で画面の文言が変わる</summary>
    public string Language
    {
        get => Loc.Instance.Setting;
        set
        {
            if (value == Loc.Instance.Setting) return;
            Loc.Instance.SetLanguage(value);
            OnPropertyChanged();
            SaveUiSettings();
        }
    }

    private JsonNode? _lastState;

    /// <summary>C# で組み立てている文言を作り直す (XAML の {l:Tr} は自分で変わる)</summary>
    private void OnLanguageChanged()
    {
        foreach (var ch in Channels) ch.OnLanguageChanged();
        OnPropertyChanged(nameof(ConnectionText));
        if (_lastState != null) OnState(_lastState, relabel: true);
        _ = SampleSystemAsync();
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
            Log(T("event.vcamRegistered"));
        }
        else
        {
            Log(F("event.vcamRegisterFailed", ("error", error ?? T("vcam.notConfirmed"))), true);
        }
    }

    private void UnregisterVcam()
    {
        var answer = MessageBox.Show(Application.Current.MainWindow, T("dialog.unregisterVcam"),
            "denpachan", MessageBoxButton.OKCancel, MessageBoxImage.Question);
        if (answer != MessageBoxResult.OK) return;
        var error = VcamRegistration.UnregisterForUser();
        Log(error == null ? T("event.vcamUnregistered") : F("event.vcamUnregisterFailed", ("error", error)), error != null);
    }

    /// <summary>全チャンネル (上限の数。番号 - 1 で引く)</summary>
    public ObservableCollection<ChannelViewModel> Channels { get; } = [];
    /// <summary>画面に出すチャンネル (先頭から全体設定の channelCount 個)</summary>
    public ObservableCollection<ChannelViewModel> VisibleChannels { get; } = [];
    public ObservableCollection<EventItem> Events { get; } = [];
    public List<AudioDeviceItem> AudioDevices { get; private set; } = [];

    private JsonNode? _generalConfig;
    public JsonNode? GeneralConfig
    {
        get => _generalConfig;
        private set
        {
            _generalConfig = value;
            UpdateVisibleChannels();
        }
    }

    /// <summary>使うチャンネル数 (全体設定の channelCount。engine の設定がまだ無ければ 4)</summary>
    public int ChannelCount => Math.Clamp(GeneralConfig.Int("channelCount", 4), 1, Channels.Count);

    // タイルの並べ方: 1 → 1x1、2 → 2x1、3〜4 → 2x2、5〜6 → 3x2、7〜8 → 4x2
    public int TileColumns => ChannelCount switch { 1 => 1, <= 4 => 2, <= 6 => 3, _ => 4 };
    public int TileRows => ChannelCount <= 2 ? 1 : 2;

    private void UpdateVisibleChannels()
    {
        var count = ChannelCount;
        if (VisibleChannels.Count == count) return;
        VisibleChannels.Clear();
        foreach (var ch in Channels.Take(count)) VisibleChannels.Add(ch);
        if (Selected == null || Selected.Id > count) Selected = Channels[0];
        OnPropertyChanged(nameof(ChannelCount));
        OnPropertyChanged(nameof(TileColumns));
        OnPropertyChanged(nameof(TileRows));
    }

    public ICommand RefreshDevicesCommand { get; }
    public ICommand GeneralSettingsCommand { get; }
    public ICommand ShutdownCommand { get; }
    public ICommand SelectCommand { get; }
    public ICommand UnregisterVcamCommand { get; }
    public ICommand OpenSettingsFolderCommand { get; }

    /// <summary>設定ファイル (engine の config.json、画面の ui.json) のあるフォルダ</summary>
    private static string SettingsFolder =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "denpachan");

    /// <summary>設定ファイルのフォルダをエクスプローラーで開く (まだ無ければ作ってから開く)</summary>
    private void OpenSettingsFolder()
    {
        try
        {
            Directory.CreateDirectory(SettingsFolder);
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("explorer.exe", $"\"{SettingsFolder}\"") { UseShellExecute = true });
        }
        catch (Exception e)
        {
            Log(F("event.openFolderFailed", ("path", SettingsFolder), ("error", e.Message)), true);
        }
    }

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
    public string ConnectionText => T(Connected ? "main.status.engineConnected" : "main.status.engineDisconnected");

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
            // 録画先: 使っている (画面に出している) チャンネルのうち、録画がオンのもののフォルダ (空 = Videos\denpachan)。
            // 無ければシステムドライブ。使っていないチャンネルは録画しないので入れない
            var drives = VisibleChannels.Where(c => c.Config != null && c.Config.At("record").Bool("enabled"))
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
            SystemText = F(disks.Count > 0 ? "main.status.systemWithDisks" : "main.status.system", ("cpu", $"{s.CpuPercent:0}"),
                           ("memory", FormatGb(s.MemoryFreeGb)), ("disks", string.Join(", ", disks)));
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
        Log(T(connected ? "event.engineConnected" : "event.engineDisconnected"), !connected);
        CommandManager.InvalidateRequerySuggested();
        if (connected)
        {
            _ = LoadDevicesAsync();
            _ = LoadConfigAsync();
        }
        else
        {
            foreach (var ch in Channels) ch.SetEngineDisconnected();
        }
    }

    /// <summary>relabel = 言語を変えたので文言だけ作り直す (イベントログには出さない)</summary>
    private void OnState(JsonNode state, bool relabel = false)
    {
        _lastState = state;
        if (state.At("channels") is JsonArray channels)
        {
            foreach (var st in channels)
            {
                var id = st.Int("id");
                if (id < 1 || id > Channels.Count) continue;
                var ch = Channels[id - 1];
                foreach (var e in ch.ApplyState(st!, relabel)) Log(F("event.channel", ("name", ch.Name), ("text", e.Text)), e.Warning);
            }
        }
        var ffmpeg = state.At("ffmpeg");
        var missing = ffmpeg != null && !ffmpeg.Bool("ok", true);
        var ffmpegMessage = Message(ffmpeg.At("message"));
        if (missing != FfmpegMissing)
        {
            if (!relabel) Log(missing ? ffmpegMessage : T("event.ffmpegFound"), missing);
            FfmpegMissing = missing;
        }
        FfmpegMessage = missing ? F("main.ffmpegBanner", ("message", ffmpegMessage)) : "";
        var rtmp = state.At("rtmpServer");
        var rtmpState = rtmp.Str("state", "stopped");
        RtmpText = rtmpState switch
        {
            "stopped" => T("main.status.rtmpStopped"),
            "running" => F("main.status.rtmpRunning", ("port", rtmp.Int("port"))),
            _ => F("main.status.rtmpRestarting", ("port", rtmp.Int("port"))),
        };
        var restarts = (long)rtmp.Num("restarts");
        if (!relabel && _lastRtmpRestarts >= 0 && restarts > _lastRtmpRestarts)
        {
            Log(F("event.rtmpRestarted", ("reason", Message(rtmp.At("lastEvent")))), true);
        }
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
            Log(F("event.configLoadFailed", ("error", e.Message)), true);
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
            Log(F("event.devicesFailed", ("error", e.Message)), true);
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
            Log(F("event.callFailed", ("error", e.Message)), true);
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
        Log(F("event.patchFailed", ("error", error)), true);
        MessageBox.Show(Application.Current.MainWindow, error, "denpachan", MessageBoxButton.OK, MessageBoxImage.Warning);
        return false;
    }

    public async Task<string?> TryPatchGeneralAsync(JsonObject patch)
    {
        try
        {
            GeneralConfig = await _client.CallAsync("patchGeneral", new JsonObject { ["patch"] = patch });
            // チャンネル数を増やすと、engine が新しく使うチャンネルのポートやカメラを付け替えることがあるので読み直す
            await LoadConfigAsync();
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
        var names = string.Join(T("common.listSeparator"), active.Select(c => c.Name));
        var answer = MessageBox.Show(Application.Current.MainWindow, F("dialog.closeWhileReceiving", ("names", names)),
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

    // ---- 画面の設定 (プレビューのオン・オフ、操作ロック、言語) ----

    private static string UiSettingsPath => Path.Combine(SettingsFolder, "ui.json");

    private bool _loadingUiSettings;

    private void LoadUiSettings()
    {
        // 言語は ui.json が無くても (初回起動) 「自動」で決める (決めないと、英語の環境でも日本語で出る)
        Loc.Instance.SetLanguage(Loc.Auto);
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
            Loc.Instance.SetLanguage(json.Str("language", Loc.Auto));
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
                ["language"] = Loc.Instance.Setting,
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
