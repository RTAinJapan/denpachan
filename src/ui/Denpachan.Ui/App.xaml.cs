using System.IO;
using System.Text.Json.Nodes;
using System.Windows;
using Denpachan.Ui.Infrastructure;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;

namespace Denpachan.Ui;

/// <summary>
/// 起動オプション:
///   --screenshot PATH [--delay SEC]   試験用: 指定秒数後に画面を PNG に保存して終了する
///   --select N                         試験用: 起動時に選択するチャンネル
///   --settings N [--settings-tab T]    試験用: --screenshot で、チャンネル N の設定画面 (T 番目のタブ、0 から) を撮る
///   --general                          試験用: --screenshot で、全体の設定画面を撮る
///   --size WxH                         試験用: --screenshot で、ウィンドウをこの大きさにする (枠なし・サイズ変更不可にして、
///                                      CI の小さい画面でも Windows に縮められないようにする)。H が 0 なら中身の自然な高さ (設定画面)
///   --fixture PATH                     試験用: engine に繋がず、ファイルの設定と状態を画面に流し込む (UiFixture)。
///                                      --screenshot と使うと、PNG の隣に画面の状態 (PATH.txt) を書き、
///                                      fixture の expect と合わなければ終了コード 1
/// </summary>
public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        MainWindow? window = null;
        string? screenshot = null;
        var inHandler = false;
        var lastError = "";
        var lastErrorAt = DateTime.MinValue;
        DispatcherUnhandledException += (_, args) =>
        {
            // UI の不具合で配信を止めないよう、落とさずに知らせる (engine / worker は別プロセス)。
            // 毎秒の状態更新で同じ例外が続くと、ダイアログの中でまた例外が起きてダイアログが積み重なるので、
            // 表示中・同じ内容が続く間はイベントログに出すだけにする (ログ中の例外で再入しても何もしない)
            args.Handled = true;
            if (inHandler) return;
            inHandler = true;
            try
            {
                var message = args.Exception.Message;
                if (window?.ViewModel.FixtureMode == true)
                {
                    // 試験 (CI) ではダイアログを待つ人がいない: 例外を書き出して終わる
                    if (screenshot != null) File.WriteAllText(screenshot + ".txt", "{\"result\": \"exception\", \"error\": " + System.Text.Json.JsonSerializer.Serialize(args.Exception.ToString()) + "}");
                    Shutdown(2);
                    return;
                }
                var repeated = message == lastError && DateTime.Now - lastErrorAt < TimeSpan.FromMinutes(1);
                lastError = message;
                lastErrorAt = DateTime.Now;
                try { window?.ViewModel.Log($"画面のエラー: {message}", true); } catch { }
                if (!repeated) MessageBox.Show(message, "denpachan", MessageBoxButton.OK, MessageBoxImage.Error);
            }
            finally
            {
                inHandler = false;
            }
        };

        window = new MainWindow();
        var delay = 6.0;
        var settingsFor = 0;  // 試験用: このチャンネルの設定画面を撮る (0 = メイン画面)
        var settingsTab = 0;
        var general = false;
        Size? size = null;
        JsonNode? fixture = null;
        for (var i = 0; i < e.Args.Length; i++)
        {
            switch (e.Args[i])
            {
                case "--fixture" when i + 1 < e.Args.Length:
                    fixture = UiFixture.Load(e.Args[++i]);
                    UiFixture.Apply(window.ViewModel, fixture);
                    break;
                case "--screenshot" when i + 1 < e.Args.Length: screenshot = e.Args[++i]; break;
                case "--delay" when i + 1 < e.Args.Length: delay = double.Parse(e.Args[++i]); break;
                case "--settings" when i + 1 < e.Args.Length: settingsFor = int.Parse(e.Args[++i]); break;
                case "--settings-tab" when i + 1 < e.Args.Length: settingsTab = int.Parse(e.Args[++i]); break;
                case "--general": general = true; break;
                case "--size" when i + 1 < e.Args.Length:
                    var wh = e.Args[++i].Split('x');
                    size = new Size(double.Parse(wh[0]), double.Parse(wh[1]));
                    break;
                case "--select" when i + 1 < e.Args.Length:
                    var n = int.Parse(e.Args[++i]);
                    if (window.ViewModel.Channels.Count >= n) window.ViewModel.Selected = window.ViewModel.Channels[n - 1];
                    break;
            }
        }
        // 枠のあるウィンドウは画面より大きくできない (Windows が縮める) ので、撮影のときは枠なしにして大きさを決める
        void ApplySize(Window w)
        {
            if (size is not { } s) return;
            w.WindowStyle = WindowStyle.None;
            w.ResizeMode = ResizeMode.NoResize;
            w.Width = s.Width;
            if (s.Height > 0)
            {
                w.SizeToContent = SizeToContent.Manual;
                w.Height = s.Height;
            }
            else
            {
                w.SizeToContent = SizeToContent.Height;
            }
        }
        if (screenshot != null) ApplySize(window);
        window.Show();

        if (screenshot != null)
        {
            var timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(delay) };
            timer.Tick += (_, _) =>
            {
                timer.Stop();
                Window target = window;
                if (settingsFor > 0 && window.ViewModel.Channels[settingsFor - 1].Config != null)
                {
                    var settings = new Views.ChannelSettingsWindow(window.ViewModel, window.ViewModel.Channels[settingsFor - 1]) { Owner = window };
                    settings.Tabs.SelectedIndex = settingsTab;
                    target = settings;
                    ApplySize(target);
                    target.Show();
                    target.UpdateLayout();
                }
                else if (general && window.ViewModel.GeneralConfig != null)
                {
                    target = new Views.GeneralSettingsWindow(window.ViewModel, window.ViewModel.GeneralConfig) { Owner = window };
                    ApplySize(target);
                    target.Show();
                    target.UpdateLayout();
                }
                Dispatcher.InvokeAsync(() =>
                {
                    SaveScreenshot(target, screenshot);
                    var exitCode = 0;
                    if (fixture != null)
                    {
                        var failures = UiFixture.Check(window.ViewModel, fixture["expect"]);
                        File.WriteAllText(screenshot + ".txt", UiFixture.Report(window.ViewModel, failures));
                        exitCode = failures.Count == 0 ? 0 : 1;
                    }
                    Shutdown(exitCode);
                }, DispatcherPriority.ContextIdle);
            };
            timer.Start();
        }
    }

    private static void SaveScreenshot(Window window, string path)
    {
        // ウィンドウの背景の上に中身を描く (中身だけを描くと背景が透明になるため)
        var content = (FrameworkElement)window.Content;
        var dpi = VisualTreeHelper.GetDpi(window);
        var size = new Size(content.ActualWidth, content.ActualHeight);
        var visual = new DrawingVisual();
        using (var dc = visual.RenderOpen())
        {
            dc.DrawRectangle(window.Background, null, new Rect(size));
            dc.DrawRectangle(new VisualBrush(content), null, new Rect(size));
        }
        var bitmap = new RenderTargetBitmap((int)(size.Width * dpi.DpiScaleX), (int)(size.Height * dpi.DpiScaleY),
                                            dpi.PixelsPerInchX, dpi.PixelsPerInchY, PixelFormats.Pbgra32);
        bitmap.Render(visual);
        var encoder = new PngBitmapEncoder();
        encoder.Frames.Add(BitmapFrame.Create(bitmap));
        using var file = File.Create(path);
        encoder.Save(file);
    }
}
