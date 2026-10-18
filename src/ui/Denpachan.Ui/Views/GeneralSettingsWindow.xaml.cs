using System.Text.Json.Nodes;
using System.Windows;
using Denpachan.Ui.Infrastructure;
using Denpachan.Ui.ViewModels;

namespace Denpachan.Ui.Views;

/// <summary>全体の設定</summary>
public partial class GeneralSettingsWindow : Window
{
    private readonly MainViewModel _main;

    public GeneralSettingsWindow(MainViewModel main, JsonNode general)
    {
        InitializeComponent();
        _main = main;
        AvSyncBox.IsChecked = general.Bool("avSync", true);
        MarginBox.Text = general.Int("audioMarginMs", 10).ToString();
        StallBox.Text = general.Int("stallMs", 2000).ToString();
        var rtmp = general.At("rtmpServer");
        BindBox.Text = rtmp.Str("bind", "0.0.0.0");
        RtmpPortBox.Text = rtmp.Int("port", 1935).ToString();
        IdleBox.Text = rtmp.Int("idleTimeoutMs", 5000).ToString();
    }

    private async void Save_Click(object sender, RoutedEventArgs e)
    {
        if (!int.TryParse(MarginBox.Text, out var margin) || !int.TryParse(StallBox.Text, out var stall) ||
            !int.TryParse(RtmpPortBox.Text, out var port) || !int.TryParse(IdleBox.Text, out var idle))
        {
            ShowError("数値で入力してください");
            return;
        }
        var patch = new JsonObject
        {
            ["avSync"] = AvSyncBox.IsChecked == true,
            ["audioMarginMs"] = margin,
            ["stallMs"] = stall,
            ["rtmpServer"] = new JsonObject { ["bind"] = BindBox.Text.Trim(), ["port"] = port, ["idleTimeoutMs"] = idle },
        };
        SaveButton.IsEnabled = false;
        var error = await _main.TryPatchGeneralAsync(patch);
        SaveButton.IsEnabled = true;
        if (error != null)
        {
            ShowError(error);
            return;
        }
        _main.Log("全体の設定を変更しました");
        DialogResult = true;
    }

    private void ShowError(string message)
    {
        ErrorText.Text = message;
        ErrorText.Visibility = Visibility.Visible;
    }
}
