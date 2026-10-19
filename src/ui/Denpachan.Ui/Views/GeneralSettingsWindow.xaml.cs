using System.Text.Json.Nodes;
using System.Windows;
using Denpachan.Ui.Infrastructure;
using Denpachan.Ui.ViewModels;

namespace Denpachan.Ui.Views;

/// <summary>全体設定</summary>
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
        FfmpegBox.Text = general.Str("ffmpegPath");
    }

    private void BrowseFfmpeg_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new Microsoft.Win32.OpenFileDialog { Title = "ffmpeg.exe", Filter = "ffmpeg.exe|ffmpeg.exe|実行ファイル (*.exe)|*.exe" };
        if (dialog.ShowDialog(this) == true) FfmpegBox.Text = dialog.FileName;
    }

    private async void CheckFfmpeg_Click(object sender, RoutedEventArgs e)
    {
        FfmpegStatus.Text = "確認しています…";
        FfmpegStatus.Foreground = (System.Windows.Media.Brush)FindResource("DimTextBrush");
        var result = await ExternalFfmpeg.CheckAsync(FfmpegBox.Text.Trim());
        FfmpegStatus.Text = result.Message;
        FfmpegStatus.Foreground = result.Ok ? (System.Windows.Media.Brush)FindResource("TextBrush") : System.Windows.Media.Brushes.LightCoral;
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
            ["ffmpegPath"] = FfmpegBox.Text.Trim(),
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
        _main.Log("全体設定を変更しました");
        DialogResult = true;
    }

    private void ShowError(string message)
    {
        ErrorText.Text = message;
        ErrorText.Visibility = Visibility.Visible;
    }
}
