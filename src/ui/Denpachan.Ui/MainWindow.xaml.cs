using System.Windows;
using System.Windows.Input;
using Denpachan.Ui.ViewModels;

namespace Denpachan.Ui;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
        ViewModel = new MainViewModel();
        DataContext = ViewModel;
        Loaded += (_, _) => ViewModel.Start();
        Closed += (_, _) => ViewModel.Dispose();  // engine は止めない (受信は続ける)
    }

    public MainViewModel ViewModel { get; }

    private void Tile_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (sender is FrameworkElement { DataContext: ChannelViewModel ch }) ViewModel.Selected = ch;
    }

    private void Close_Click(object sender, RoutedEventArgs e) => Close();
}
