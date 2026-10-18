using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text.Json.Nodes;
using System.Windows;
using System.Windows.Controls;
using Denpachan.Ui.Infrastructure;
using Denpachan.Ui.ViewModels;

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
        Title = $"{channel.Name} の設定";

        var c = channel.Config!;
        var s = c.At("source");
        NameBox.Text = c.Str("name");
        Select(InputBox, $"{s.Str("protocol", "srt")}:{s.Str("mode", "listener")}");
        HostBox.Text = s.Str("host", "0.0.0.0");
        PortBox.Text = s.Int("port").ToString();
        LatencyBox.Text = s.Int("latencyMs", 120).ToString();
        PassphraseHint.Text = s.Bool("hasPassphrase") ? "設定済み (空欄なら変更しない)" : "未設定";
        Select(PbkeylenBox, s.Int("pbkeylen").ToString());
        StreamIdBox.Text = s.Str("streamId");
        AppBox.Text = s.Str("app", "live");
        KeyBox.Text = s.Str("key");
        UrlBox.Text = s.Str("url");
        BufferBox.Text = c.Int("playoutBufferMs").ToString();
        var a = c.At("audio");
        Select(AudioModeBox, a.Str("mode", "shared"));
        OffsetBox.Text = a.Int("offsetMs").ToString();
        FadeBox.IsChecked = a.Bool("fade", true);
        UpdatePanels();
        UpdatePushUrl();
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
        HostLabel.Content = input == "srt:caller" ? "接続先" : "待ち受けアドレス";
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

    private static bool TryInt(TextBox box, string name, out int value, List<string> errors)
    {
        if (int.TryParse(box.Text.Trim(), out value)) return true;
        errors.Add($"{name} は数値で入力してください");
        return false;
    }

    private async void Save_Click(object sender, RoutedEventArgs e)
    {
        var errors = new List<string>();
        TryInt(PortBox, "ポート", out var port, errors);
        TryInt(LatencyBox, "latency", out var latency, errors);
        TryInt(BufferBox, "再生バッファ", out var buffer, errors);
        TryInt(OffsetBox, "音声オフセット", out var offset, errors);
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
            ["latencyMs"] = latency,
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
            ["source"] = source,
            ["playoutBufferMs"] = buffer,
            ["audio"] = new JsonObject
            {
                ["mode"] = SelectedTag(AudioModeBox),
                ["offsetMs"] = offset,
                ["fade"] = FadeBox.IsChecked == true,
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
        _main.Log($"{_channel.Name}: 設定を変更しました");
        DialogResult = true;
    }

    private void ShowError(string message)
    {
        ErrorText.Text = message;
        ErrorText.Visibility = Visibility.Visible;
    }
}
