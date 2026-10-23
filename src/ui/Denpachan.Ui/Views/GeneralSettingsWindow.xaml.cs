using System.Text.Json.Nodes;
using System.Windows;
using Denpachan.Ui.Infrastructure;
using Denpachan.Ui.ViewModels;
using static Denpachan.Ui.Infrastructure.Loc;

namespace Denpachan.Ui.Views;

/// <summary>全体設定</summary>
public partial class GeneralSettingsWindow : Window
{
    private readonly MainViewModel _main;

    public GeneralSettingsWindow(MainViewModel main, JsonNode general)
    {
        InitializeComponent();
        _main = main;
        // 言語は画面の設定 (ui.json)。ほかは engine の設定
        LanguageBox.ItemsSource = new List<Option<string>> { new(Auto, T("general.languageAuto")) }
            .Concat(Instance.Languages.Select(l => new Option<string>(l.Code, l.Name)))
            .ToList();
        LanguageBox.SelectedValue = main.Language;
        if (LanguageBox.SelectedIndex < 0) LanguageBox.SelectedIndex = 0;
        ChannelCountBox.ItemsSource = Enumerable.Range(1, VcamRegistration.Count).ToList();
        ChannelCountBox.SelectedItem = main.ChannelCount;
        AvSyncBox.IsChecked = general.Bool("avSync", true);
        MarginBox.Text = general.Int("audioMarginMs", 10).ToString();
        StallBox.Text = general.Int("stallMs", 2000).ToString();
        var rtmp = general.At("rtmpServer");
        BindBox.Text = rtmp.Str("bind", "0.0.0.0");
        RtmpPortBox.Text = rtmp.Int("port", 1935).ToString();
        IdleBox.Text = rtmp.Int("idleTimeoutMs", 5000).ToString();
        var metrics = general.At("metrics");
        MetricsEnabledBox.IsChecked = metrics.Bool("enabled");
        MetricsBindBox.Text = metrics.Str("bind", "127.0.0.1");
        MetricsPortBox.Text = metrics.Int("port", 9100).ToString();
        MetricsAllowControlBox.IsChecked = metrics.Bool("allowControl");
        // トークンは engine から返らない (設定済みかどうかだけ)。作り直したときだけ値を出す
        MetricsTokenBox.Text = T(metrics.Bool("hasToken") ? "general.metricsTokenSet" : "general.metricsTokenUnset");
    }

    private string? _newToken;  // 作り直したトークン (保存するまで)

    private void RegenerateToken_Click(object sender, RoutedEventArgs e)
    {
        // URL にそのまま入れられる文字だけ (Base64url、32 バイト = 43 文字)
        _newToken = Convert.ToBase64String(System.Security.Cryptography.RandomNumberGenerator.GetBytes(32))
            .TrimEnd('=').Replace('+', '-').Replace('/', '_');
        MetricsTokenBox.Text = _newToken;
        MetricsTokenHint.Text = T("general.metricsTokenNew");
        MetricsTokenBox.Focus();
        MetricsTokenBox.SelectAll();
    }

    private async void Save_Click(object sender, RoutedEventArgs e)
    {
        if (!int.TryParse(MarginBox.Text, out var margin) || !int.TryParse(StallBox.Text, out var stall) ||
            !int.TryParse(RtmpPortBox.Text, out var port) || !int.TryParse(IdleBox.Text, out var idle) ||
            !int.TryParse(MetricsPortBox.Text, out var metricsPort))
        {
            ShowError(T("general.notNumber"));
            return;
        }
        // チャンネル数を減らして、外れるチャンネルが受信中なら止めてよいか聞く (engine が止める。既定はキャンセル)
        var count = ChannelCountBox.SelectedItem is int n ? n : _main.ChannelCount;
        var stopping = _main.Channels.Where(c => c.Id > count && c.Enabled).Select(c => c.Name).ToList();
        if (stopping.Count > 0)
        {
            var answer = MessageBox.Show(this, F("dialog.channelCountStop", ("channels", string.Join(T("common.listSeparator"), stopping))),
                "denpachan", MessageBoxButton.OKCancel, MessageBoxImage.Warning, MessageBoxResult.Cancel);
            if (answer != MessageBoxResult.OK) return;
        }
        // 言語はその場で変える (engine の設定が保存できなくても、画面の言語は変わる)
        if (LanguageBox.SelectedValue is string language) _main.Language = language;
        var patch = new JsonObject
        {
            ["channelCount"] = count,
            ["avSync"] = AvSyncBox.IsChecked == true,
            ["audioMarginMs"] = margin,
            ["stallMs"] = stall,
            ["rtmpServer"] = new JsonObject { ["bind"] = BindBox.Text.Trim(), ["port"] = port, ["idleTimeoutMs"] = idle },
        };
        var metrics = new JsonObject
        {
            ["enabled"] = MetricsEnabledBox.IsChecked == true,
            ["bind"] = MetricsBindBox.Text.Trim(),
            ["port"] = metricsPort,
            ["allowControl"] = MetricsAllowControlBox.IsChecked == true,
        };
        if (_newToken != null) metrics["token"] = _newToken;  // 作り直したときだけ送る (送らなければ今のトークンのまま)
        patch["metrics"] = metrics;
        SaveButton.IsEnabled = false;
        var error = await _main.TryPatchGeneralAsync(patch);
        SaveButton.IsEnabled = true;
        if (error != null)
        {
            ShowError(error);
            return;
        }
        _main.Log(T("event.generalSettingsSaved"));
        DialogResult = true;
    }

    private void ShowError(string message)
    {
        ErrorText.Text = message;
        ErrorText.Visibility = Visibility.Visible;
    }
}
