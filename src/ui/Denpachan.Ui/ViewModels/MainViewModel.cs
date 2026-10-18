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

        RefreshDevicesCommand = new RelayCommand(() => _ = LoadDevicesAsync(), () => Connected);
        GeneralSettingsCommand = new RelayCommand(OpenGeneralSettings, () => Connected && GeneralConfig != null);
        ShutdownCommand = new RelayCommand(() => _ = ShutdownAsync(), () => Connected);
        SelectCommand = new RelayCommand(p => { if (p is ChannelViewModel ch) Selected = ch; });
    }

    public void Start()
    {
        _client.Start();
        _previewTimer.Start();
    }

    public ObservableCollection<ChannelViewModel> Channels { get; } = [];
    public ObservableCollection<EventItem> Events { get; } = [];
    public List<AudioDeviceItem> AudioDevices { get; private set; } = [];
    public JsonNode? GeneralConfig { get; private set; }

    public ICommand RefreshDevicesCommand { get; }
    public ICommand GeneralSettingsCommand { get; }
    public ICommand ShutdownCommand { get; }
    public ICommand SelectCommand { get; }

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

    private async Task ShutdownAsync()
    {
        var answer = MessageBox.Show(Application.Current.MainWindow,
            "engine と全チャンネルの受信を停止して終了します。\n仮想カメラと音声出力も止まります。よろしいですか?",
            "denpachan", MessageBoxButton.OKCancel, MessageBoxImage.Warning);
        if (answer != MessageBoxResult.OK) return;
        await CallAsync("shutdown");
        Application.Current.Shutdown();
    }

    // ---- イベントログ ----

    public void Log(string text, bool warning = false)
    {
        Events.Insert(0, new EventItem(DateTime.Now, text, warning));
        while (Events.Count > 500) Events.RemoveAt(Events.Count - 1);
    }

    // ---- 画面の設定 (プレビューのオン・オフ) ----

    private static string UiSettingsPath =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "denpachan", "ui.json");

    private bool _loadingUiSettings;

    private void LoadUiSettings()
    {
        try
        {
            if (!File.Exists(UiSettingsPath)) return;
            var json = JsonNode.Parse(File.ReadAllText(UiSettingsPath));
            if (json.At("preview") is JsonArray preview)
            {
                _loadingUiSettings = true;
                for (var i = 0; i < Math.Min(preview.Count, Channels.Count); i++) Channels[i].PreviewEnabled = preview[i]?.GetValue<bool>() ?? true;
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
        if (_loadingUiSettings) return;
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(UiSettingsPath)!);
            var json = new JsonObject { ["preview"] = new JsonArray(Channels.Select(c => (JsonNode)c.PreviewEnabled).ToArray()) };
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
        _client.Dispose();
        foreach (var ch in Channels) ch.Dispose();
    }
}
