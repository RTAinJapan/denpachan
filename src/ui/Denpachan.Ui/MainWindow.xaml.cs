using System.ComponentModel;
using System.Windows;
using System.Windows.Input;
using System.Windows.Threading;
using Denpachan.Ui.ViewModels;

namespace Denpachan.Ui;

public partial class MainWindow : Window
{
    private bool _closeConfirmed;
    private bool _closing;  // 閉じる手順の途中 (繋がるのを待っている、確認を出している)
    private DispatcherTimer? _sessionEndingTimer;

    public MainWindow()
    {
        InitializeComponent();
        Title = $"denpachan {Infrastructure.AppVersion.Text}";  // 版はタイトルに出す (問い合わせのときに確かめられるように)
        ViewModel = new MainViewModel();
        DataContext = ViewModel;
        Loaded += (_, _) => ViewModel.Start();
        Closing += OnClosing;
        Closed += (_, _) => ViewModel.Dispose();
    }

    public MainViewModel ViewModel { get; }

    /// <summary>確認なしで閉じる (試験で使う。確認も engine の停止もしない)</summary>
    public void AllowCloseWithoutConfirm() => _closeConfirmed = true;

    /// <summary>
    /// Windows のシャットダウン・ログオフ: 確認なしで閉じる (ダイアログでシャットダウンを止めないように)。
    /// ほかのアプリがシャットダウンを取り消すと画面は残るので、1 分経ったら元に戻す (戻さないと、以後は確認なしで閉じ、
    /// engine も止めずに残す)
    /// </summary>
    public void AllowCloseForSessionEnding()
    {
        _closeConfirmed = true;
        _sessionEndingTimer?.Stop();
        _sessionEndingTimer = new DispatcherTimer { Interval = TimeSpan.FromMinutes(1) };
        _sessionEndingTimer.Tick += (_, _) =>
        {
            _sessionEndingTimer.Stop();
            _closeConfirmed = false;
        };
        _sessionEndingTimer.Start();
    }

    // 閉じると受信も止める (engine を worker ごと止める)。受信中なら閉じてよいかを聞く
    private async void OnClosing(object? sender, CancelEventArgs e)
    {
        // 試験用の状態 (fixture) では engine につながっていないので、確かめず止めもしない
        if (_closeConfirmed || ViewModel.FixtureMode) return;
        e.Cancel = true;
        if (_closing) return;  // 待っている間にもう一度閉じようとした
        _closing = true;
        try
        {
            // engine と繋がっていなければ少し待つ (起動し直した直後など)。繋がらないまま閉じると、engine が残っていても
            // 止められず、画面なしで受信を続けてしまう
            Cursor = Cursors.Wait;
            var connected = await ViewModel.PrepareCloseAsync(TimeSpan.FromSeconds(5));
            Cursor = null;
            if (connected)
            {
                if (!ViewModel.ConfirmClose()) return;
            }
            else if (Engine.EngineLauncher.IsRunning())
            {
                // engine は動いているのに繋がらない: 止められないことを伝えて閉じる
                MessageBox.Show(this, Infrastructure.Loc.T("dialog.closeEngineUnreachable"), "denpachan", MessageBoxButton.OK,
                    MessageBoxImage.Warning);
            }
            _closeConfirmed = true;
            IsEnabled = false;
            await ViewModel.StopEngineAsync();
            // engine に繋がっていないと StopEngineAsync はすぐ終わり、まだ Closing の中にいる。ここで Close() を呼ぶと
            // WPF が例外を出し、ウィンドウが操作できないまま残るので、Closing を抜けてから閉じる
            await Dispatcher.InvokeAsync(Close, DispatcherPriority.Background);
        }
        finally
        {
            Cursor = null;
            _closing = false;
        }
    }

    private void Tile_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (sender is FrameworkElement { DataContext: ChannelViewModel ch }) ViewModel.Selected = ch;
    }
}
