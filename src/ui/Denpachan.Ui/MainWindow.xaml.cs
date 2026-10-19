using System.ComponentModel;
using System.Windows;
using System.Windows.Input;
using Denpachan.Ui.ViewModels;

namespace Denpachan.Ui;

public partial class MainWindow : Window
{
    private bool _closeConfirmed;

    public MainWindow()
    {
        InitializeComponent();
        ViewModel = new MainViewModel();
        DataContext = ViewModel;
        Loaded += (_, _) => ViewModel.Start();
        Closing += OnClosing;
        Closed += (_, _) => ViewModel.Dispose();
    }

    public MainViewModel ViewModel { get; }

    /// <summary>確認なしで閉じる (Windows のシャットダウン・ログオフのとき。ダイアログで止めないように)</summary>
    public void AllowCloseWithoutConfirm() => _closeConfirmed = true;

    // 閉じると受信も止める (engine を worker ごと止める)。受信中なら閉じてよいかを聞く
    private async void OnClosing(object? sender, CancelEventArgs e)
    {
        // 試験用の状態 (fixture) では engine につながっていないので、確かめず止めもしない
        if (_closeConfirmed || ViewModel.FixtureMode) return;
        e.Cancel = true;
        if (!ViewModel.ConfirmClose()) return;
        _closeConfirmed = true;
        IsEnabled = false;
        await ViewModel.StopEngineAsync();
        Close();
    }

    private void Tile_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (sender is FrameworkElement { DataContext: ChannelViewModel ch }) ViewModel.Selected = ch;
    }
}
