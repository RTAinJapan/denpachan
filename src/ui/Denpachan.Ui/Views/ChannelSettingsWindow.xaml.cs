using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text.Json.Nodes;
using System.Windows;
using System.Windows.Controls;
using Denpachan.Ui.Infrastructure;
using Denpachan.Ui.ViewModels;
using static Denpachan.Ui.Infrastructure.Loc;

namespace Denpachan.Ui.Views;

/// <summary>チャンネルの受信設定</summary>
public partial class ChannelSettingsWindow : Window
{
    private readonly MainViewModel _main;
    private readonly ChannelViewModel _channel;
    private readonly int _rtmpPort;

    public ChannelSettingsWindow(MainViewModel main, ChannelViewModel channel)
    {
        InitializeComponent();
        _main = main;
        _channel = channel;
        _rtmpPort = main.GeneralConfig.At("rtmpServer").Int("port", 1935);
        Title = F("channel.title", ("name", channel.Name));

        var c = channel.Config!;
        var s = c.At("source");
        NameBox.Text = c.Str("name");
        TestSlotBox.IsChecked = c.Bool("testSlot");
        Select(InputBox, $"{s.Str("protocol", "srt")}:{s.Str("mode", "listener")}");
        HostBox.Text = s.Str("host", "0.0.0.0");
        PortBox.Text = s.Int("port").ToString();
        LatencyBox.Text = SrtUrl.UsFromMs(s.Int("latencyMs", 120)).ToString();  // URL と同じマイクロ秒で出す
        if (channel.SrtRecommendation.Length > 0)
        {
            LatencyAdvice.Text = F("channel.latencyAdvice", ("advice", channel.SrtRecommendation));
            ApplyLatencyButton.IsEnabled = channel.SrtRecommendedLatencyMs > 0;
        }
        else
        {
            LatencyAdvice.Text = T("channel.latencyAdviceIdle");
            ApplyLatencyButton.Visibility = Visibility.Collapsed;
        }
        PassphraseHint.Text = T(s.Bool("hasPassphrase") ? "channel.passphraseSet" : "channel.passphraseUnset");
        Select(PbkeylenBox, s.Int("pbkeylen").ToString());
        StreamIdBox.Text = s.Str("streamId");
        AppBox.Text = s.Str("app", "live");
        KeyBox.Text = s.Str("key");
        UrlBox.Text = s.Str("url");
        BufferBox.Text = c.Int("playoutBufferMs").ToString();
        var a = c.At("audio");
        Select(AudioModeBox, a.Str("mode", "shared"));
        if (a.Str("device").StartsWith("ASIO: ", StringComparison.Ordinal))
        {
            // 共有 / 排他は WASAPI のデバイスのときだけ (ASIO はドライバを直接使う)
            AudioModeBox.IsEnabled = false;
            AudioModeBox.ToolTip = T("channel.audioModeAsio");
            ToolTipService.SetShowOnDisabled(AudioModeBox, true);
        }
        OffsetBox.Text = a.Int("offsetMs").ToString();
        FadeBox.IsChecked = a.Bool("fade", true);

        var r = c.At("record");
        var v = r.At("encode").At("video");
        RecordEnabledBox.IsChecked = r.Bool("enabled");
        RecordDirBox.Text = r.Str("dir");
        TemplateBox.Text = r.Str("template", "{ch} - {YYYY}{MM}/{DD} {hh}-{mm}-{ss}");
        ReplacementBox.Text = r.Str("invalidCharReplacement", "-");
        Select(ContainerBox, r.Str("container", "mkv"));
        GapBox.Text = r.Int("gapTimeoutSec", 10).ToString();
        MaxSegmentBox.Text = r.Int("maxSegmentMin").ToString();
        MinFreeBox.Text = r.Int("minFreeGb", 5).ToString();
        var encoder = v.Str("encoder", "auto");
        (r.Str("mode", "passthrough") != "encode" ? PassthroughBox : encoder == "cpu" ? CpuEncodeBox : EncodeBox).IsChecked = true;
        Select(CodecBox, v.Str("codec", "h264"));
        Select(EncoderBox, encoder);
        Select(RcBox, v.Str("rc", "cbr"));
        BitrateBox.Text = v.Int("bitrateKbps", 12000).ToString();
        QpBox.Text = v.Int("qp", 23).ToString();
        Select(PresetBox, v.Str("preset", "p4"));
        GopBox.Text = v.Int("gopSec", 2).ToString();
        AudioBitrateBox.Text = r.At("encode").At("audio").Int("bitrateKbps", 192).ToString();

        UpdatePanels();
        UpdatePushUrl();
        UpdateTemplatePreview();
        UpdateEncodePanel();
    }

    // ---- 録画 ----

    private void Mode_Changed(object sender, RoutedEventArgs e) => UpdateEncodePanel();

    private void UpdateEncodePanel()
    {
        if (EncodePanel == null || EncoderBox == null || CpuNote == null) return;
        var gpu = EncodeBox.IsChecked == true;
        var cpu = CpuEncodeBox.IsChecked == true;
        EncodePanel.IsEnabled = gpu || cpu;
        EncoderBox.Visibility = cpu ? Visibility.Collapsed : Visibility.Visible;
        CpuNote.Visibility = cpu ? Visibility.Visible : Visibility.Collapsed;
    }

    private void Template_Changed(object sender, RoutedEventArgs e) => UpdateTemplatePreview();

    private void UpdateTemplatePreview()
    {
        if (TemplatePreview == null || TemplateBox == null || ContainerBox?.SelectedItem == null) return;
        var dir = RecordDirBox.Text.Trim();
        if (dir.Length == 0) dir = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Videos", "denpachan");
        var name = RecordFilename.Expand(TemplateBox.Text, _channel.Id, NameBox.Text.Trim(), DateTime.Now, 1, "h264", 1920, 1080,
                                         ReplacementBox.Text);
        TemplatePreview.Text = System.IO.Path.Combine(dir, name) + "." + SelectedTag(ContainerBox);
    }

    private void BrowseDir_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new Microsoft.Win32.OpenFolderDialog { Title = T("record.dirDialog") };
        if (System.IO.Directory.Exists(RecordDirBox.Text)) dialog.InitialDirectory = RecordDirBox.Text;
        if (dialog.ShowDialog(this) == true) RecordDirBox.Text = dialog.FolderName;
    }

    private static void Select(ComboBox box, string tag)
    {
        foreach (ComboBoxItem item in box.Items)
        {
            if ((string)item.Tag == tag)
            {
                box.SelectedItem = item;
                return;
            }
        }
        box.SelectedIndex = 0;
    }

    private static string SelectedTag(ComboBox box) => (string)((ComboBoxItem)box.SelectedItem).Tag;

    private void InputBox_SelectionChanged(object sender, SelectionChangedEventArgs e) => UpdatePanels();

    private void UpdatePanels()
    {
        if (SrtPanel == null) return;  // InitializeComponent の途中
        var input = SelectedTag(InputBox);
        SrtPanel.Visibility = input.StartsWith("srt") ? Visibility.Visible : Visibility.Collapsed;
        PushPanel.Visibility = input == "rtmp:push" ? Visibility.Visible : Visibility.Collapsed;
        PullPanel.Visibility = input == "rtmp:pull" ? Visibility.Visible : Visibility.Collapsed;
        HostLabel.Content = T(input == "srt:caller" ? "channel.hostCaller" : "channel.hostListener");
        if (input == "rtmp:push" && string.IsNullOrEmpty(KeyBox.Text)) KeyBox.Text = NewKey();
    }

    private void PushUrl_Changed(object sender, TextChangedEventArgs e) => UpdatePushUrl();

    private void UpdatePushUrl()
    {
        if (PushUrlBox == null) return;
        PushUrlBox.Text = $"rtmp://{LocalAddress()}:{_rtmpPort}/{AppBox.Text}/{KeyBox.Text}";
    }

    private static string NewKey()
    {
        const string chars = "abcdefghijkmnpqrstuvwxyz23456789";  // 読み間違えやすい文字は除く
        return new string(RandomNumberGenerator.GetItems<char>(chars, 16));
    }

    private void GenerateKey_Click(object sender, RoutedEventArgs e) => KeyBox.Text = NewKey();

    private void ApplyLatency_Click(object sender, RoutedEventArgs e)
    {
        if (_channel.SrtRecommendedLatencyMs > 0) LatencyBox.Text = SrtUrl.UsFromMs(_channel.SrtRecommendedLatencyMs).ToString();
    }

    /// <summary>接続先の欄に SRT の URL (srt://host:port?latency=...) が入ったら、ポート・latency などの欄に分ける</summary>
    private void HostBox_LostFocus(object sender, RoutedEventArgs e) => ApplySrtUrl();

    private void ApplySrtUrl()
    {
        if (SrtUrl.Parse(HostBox.Text) is not { } url) return;
        HostBox.Text = url.Host.Length > 0 ? url.Host : "0.0.0.0";
        if (url.Port is { } port) PortBox.Text = port.ToString();
        if (url.LatencyUs is { } latency) LatencyBox.Text = latency.ToString();
        if (url.StreamId is { } streamId) StreamIdBox.Text = streamId;
        if (url.Passphrase is { Length: > 0 } passphrase) PassphraseBox.Password = passphrase;
        if (url.Pbkeylen is { } pbkeylen) Select(PbkeylenBox, pbkeylen.ToString());
    }

    private void CopyUrl_Click(object sender, RoutedEventArgs e) => Clipboard.SetText(PushUrlBox.Text);

    /// <summary>送出側に教える、この PC の IPv4 アドレス (有効な最初のもの)</summary>
    private static string LocalAddress()
    {
        try
        {
            foreach (var ni in NetworkInterface.GetAllNetworkInterfaces())
            {
                if (ni.OperationalStatus != OperationalStatus.Up || ni.NetworkInterfaceType == NetworkInterfaceType.Loopback) continue;
                if (ni.GetIPProperties().GatewayAddresses.Count == 0) continue;
                foreach (var addr in ni.GetIPProperties().UnicastAddresses)
                {
                    if (addr.Address.AddressFamily == AddressFamily.InterNetwork) return addr.Address.ToString();
                }
            }
        }
        catch
        {
            // 取れなければ localhost
        }
        return "127.0.0.1";
    }

    /// <summary>数値の欄を読む。読めなければ、その欄のエラー文 (欄ごとのキー。ラベルは流用しない) を足す</summary>
    private static bool TryInt(TextBox box, string errorKey, out int value, List<string> errors)
    {
        if (int.TryParse(box.Text.Trim(), out value)) return true;
        errors.Add(T(errorKey));
        return false;
    }

    private async void Save_Click(object sender, RoutedEventArgs e)
    {
        ApplySrtUrl();  // 欄を離れずに保存したとき
        var errors = new List<string>();
        TryInt(PortBox, "channel.invalid.port", out var port, errors);
        TryInt(LatencyBox, "channel.invalid.latency", out var latencyUs, errors);
        TryInt(BufferBox, "channel.invalid.playoutBuffer", out var buffer, errors);
        TryInt(OffsetBox, "channel.invalid.audioOffset", out var offset, errors);
        TryInt(GapBox, "channel.invalid.gap", out var gap, errors);
        TryInt(MaxSegmentBox, "channel.invalid.maxSegment", out var maxSegment, errors);
        TryInt(MinFreeBox, "channel.invalid.minFree", out var minFree, errors);
        TryInt(BitrateBox, "channel.invalid.bitrate", out var bitrate, errors);
        TryInt(QpBox, "channel.invalid.qp", out var qp, errors);
        TryInt(GopBox, "channel.invalid.gop", out var gop, errors);
        TryInt(AudioBitrateBox, "channel.invalid.audioBitrate", out var audioBitrate, errors);
        if (errors.Count > 0)
        {
            ShowError(string.Join("\n", errors));
            return;
        }

        var input = SelectedTag(InputBox).Split(':');
        var source = new JsonObject
        {
            ["protocol"] = input[0],
            ["mode"] = input[1],
            ["host"] = HostBox.Text.Trim(),
            ["port"] = port,
            ["latencyMs"] = SrtUrl.MsFromUs(latencyUs),
            ["pbkeylen"] = int.Parse(SelectedTag(PbkeylenBox)),
            ["streamId"] = StreamIdBox.Text.Trim(),
            ["app"] = AppBox.Text.Trim(),
            ["key"] = KeyBox.Text.Trim(),
            ["url"] = UrlBox.Text.Trim(),
        };
        if (ClearPassphraseBox.IsChecked == true) source["passphrase"] = "";
        else if (PassphraseBox.Password.Length > 0) source["passphrase"] = PassphraseBox.Password;

        var patch = new JsonObject
        {
            ["name"] = NameBox.Text.Trim(),
            ["testSlot"] = TestSlotBox.IsChecked == true,
            ["source"] = source,
            ["playoutBufferMs"] = buffer,
            ["audio"] = new JsonObject
            {
                ["mode"] = SelectedTag(AudioModeBox),
                ["offsetMs"] = offset,
                ["fade"] = FadeBox.IsChecked == true,
            },
            ["record"] = new JsonObject
            {
                ["enabled"] = RecordEnabledBox.IsChecked == true,
                ["dir"] = RecordDirBox.Text.Trim(),
                ["template"] = TemplateBox.Text.Trim(),
                ["invalidCharReplacement"] = ReplacementBox.Text,
                ["container"] = SelectedTag(ContainerBox),
                ["mode"] = PassthroughBox.IsChecked == true ? "passthrough" : "encode",
                ["gapTimeoutSec"] = gap,
                ["maxSegmentMin"] = maxSegment,
                ["minFreeGb"] = minFree,
                ["encode"] = new JsonObject
                {
                    ["video"] = new JsonObject
                    {
                        ["codec"] = SelectedTag(CodecBox),
                        ["encoder"] = CpuEncodeBox.IsChecked == true ? "cpu" : SelectedTag(EncoderBox),
                        ["rc"] = SelectedTag(RcBox),
                        ["bitrateKbps"] = bitrate,
                        ["qp"] = qp,
                        ["preset"] = SelectedTag(PresetBox),
                        ["gopSec"] = gop,
                    },
                    ["audio"] = new JsonObject { ["bitrateKbps"] = audioBitrate },
                },
            },
        };

        SaveButton.IsEnabled = false;
        var error = await _main.TryPatchChannelAsync(_channel.Id, patch);
        SaveButton.IsEnabled = true;
        if (error != null)
        {
            ShowError(error);
            return;
        }
        _main.Log(F("event.channelSettingsSaved", ("name", _channel.Name)));
        DialogResult = true;
    }

    private void ShowError(string message)
    {
        ErrorText.Text = message;
        ErrorText.Visibility = Visibility.Visible;
    }
}
