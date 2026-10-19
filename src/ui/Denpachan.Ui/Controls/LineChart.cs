using System.Globalization;
using System.Windows;
using System.Windows.Media;

namespace Denpachan.Ui.Controls;

/// <summary>
/// 軽量な折れ線グラフ (1 秒ごとの値、右端が最新)。Values を新しい配列に差し替えると描き直す。
/// 縦軸は 0 から表示範囲の最大値まで (Max を指定しなければ自動)。
/// </summary>
public sealed class LineChart : FrameworkElement
{
    public static readonly DependencyProperty ValuesProperty = DependencyProperty.Register(
        nameof(Values), typeof(IReadOnlyList<double>), typeof(LineChart), new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty StrokeProperty = DependencyProperty.Register(
        nameof(Stroke), typeof(Brush), typeof(LineChart), new FrameworkPropertyMetadata(Brushes.DeepSkyBlue, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty TitleProperty = DependencyProperty.Register(
        nameof(Title), typeof(string), typeof(LineChart), new FrameworkPropertyMetadata("", FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty UnitProperty = DependencyProperty.Register(
        nameof(Unit), typeof(string), typeof(LineChart), new FrameworkPropertyMetadata("", FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty MaxProperty = DependencyProperty.Register(
        nameof(Max), typeof(double), typeof(LineChart), new FrameworkPropertyMetadata(0.0, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty ShowLabelsProperty = DependencyProperty.Register(
        nameof(ShowLabels), typeof(bool), typeof(LineChart), new FrameworkPropertyMetadata(true, FrameworkPropertyMetadataOptions.AffectsRender));
    public static readonly DependencyProperty CapacityProperty = DependencyProperty.Register(
        nameof(Capacity), typeof(int), typeof(LineChart), new FrameworkPropertyMetadata(600, FrameworkPropertyMetadataOptions.AffectsRender));

    public IReadOnlyList<double>? Values { get => (IReadOnlyList<double>?)GetValue(ValuesProperty); set => SetValue(ValuesProperty, value); }
    public Brush Stroke { get => (Brush)GetValue(StrokeProperty); set => SetValue(StrokeProperty, value); }
    public string Title { get => (string)GetValue(TitleProperty); set => SetValue(TitleProperty, value); }
    public string Unit { get => (string)GetValue(UnitProperty); set => SetValue(UnitProperty, value); }
    public double Max { get => (double)GetValue(MaxProperty); set => SetValue(MaxProperty, value); }
    public bool ShowLabels { get => (bool)GetValue(ShowLabelsProperty); set => SetValue(ShowLabelsProperty, value); }
    /// <summary>横軸に並べる点の数 (600 = 10 分)</summary>
    public int Capacity { get => (int)GetValue(CapacityProperty); set => SetValue(CapacityProperty, value); }

    private static readonly Brush GridBrush = Freeze(new SolidColorBrush(Color.FromArgb(0x30, 0xFF, 0xFF, 0xFF)));
    private static readonly Brush LabelBrush = Freeze(new SolidColorBrush(Color.FromArgb(0xA0, 0xFF, 0xFF, 0xFF)));
    private static readonly Typeface LabelFace = new("Segoe UI");

    private static Brush Freeze(Brush b) { b.Freeze(); return b; }

    protected override void OnRender(DrawingContext dc)
    {
        var w = ActualWidth;
        var h = ActualHeight;
        if (w < 4 || h < 4) return;
        dc.DrawRectangle(Brushes.Transparent, null, new Rect(0, 0, w, h));  // マウス操作を受けるため

        var gridPen = new Pen(GridBrush, 1);
        for (var i = 1; i < 4; i++) dc.DrawLine(gridPen, new Point(0, h * i / 4), new Point(w, h * i / 4));

        var values = Values;
        if (values != null && Capacity > 0 && values.Count > Capacity) values = values.TakeLast(Capacity).ToArray();  // 直近 Capacity 点だけ
        var max = Max;
        if (values is { Count: > 0 })
        {
            if (max <= 0) max = Math.Max(values.Max(), 1e-9) * 1.15;
            var capacity = Math.Max(Capacity, values.Count);
            var step = w / Math.Max(1, capacity - 1);
            var x0 = w - step * (values.Count - 1);
            var geo = new StreamGeometry();
            using (var g = geo.Open())
            {
                for (var i = 0; i < values.Count; i++)
                {
                    var v = double.IsFinite(values[i]) ? values[i] : 0;
                    var p = new Point(x0 + step * i, h - Math.Clamp(v / max, 0, 1) * (h - 1));
                    if (i == 0) g.BeginFigure(p, false, false);
                    else g.LineTo(p, true, false);
                }
            }
            geo.Freeze();
            dc.DrawGeometry(null, new Pen(Stroke, 1.5) { LineJoin = PenLineJoin.Round }, geo);
        }

        if (!ShowLabels) return;
        var dpi = VisualTreeHelper.GetDpi(this).PixelsPerDip;
        var current = values is { Count: > 0 } ? values[values.Count - 1] : 0;
        var head = new FormattedText($"{Title}  {Format(current)} {Unit}", CultureInfo.CurrentUICulture, FlowDirection.LeftToRight,
                                     LabelFace, 11, LabelBrush, dpi);
        dc.DrawText(head, new Point(4, 2));
        if (max > 0)
        {
            var top = new FormattedText($"{Format(max)}", CultureInfo.CurrentUICulture, FlowDirection.LeftToRight, LabelFace, 10,
                                        LabelBrush, dpi);
            dc.DrawText(top, new Point(w - top.Width - 4, 2));
        }
    }

    private static string Format(double v) => v >= 100 ? v.ToString("0") : v >= 10 ? v.ToString("0.0") : v.ToString("0.00");
}

/// <summary>音声レベル (dBFS) の横棒。-60 dB から 0 dB。-6 dB から黄、-1 dB から赤。</summary>
public sealed class LevelMeter : FrameworkElement
{
    public static readonly DependencyProperty DbProperty = DependencyProperty.Register(
        nameof(Db), typeof(double), typeof(LevelMeter), new FrameworkPropertyMetadata(-100.0, FrameworkPropertyMetadataOptions.AffectsRender));

    public double Db { get => (double)GetValue(DbProperty); set => SetValue(DbProperty, value); }

    private static readonly Brush Back = Freeze(new SolidColorBrush(Color.FromRgb(0x1A, 0x1B, 0x1E)));
    private static readonly Brush Green = Freeze(new SolidColorBrush(Color.FromRgb(0x3B, 0xC1, 0x6E)));
    private static readonly Brush Yellow = Freeze(new SolidColorBrush(Color.FromRgb(0xE8, 0xC2, 0x3A)));
    private static readonly Brush Red = Freeze(new SolidColorBrush(Color.FromRgb(0xE5, 0x48, 0x4D)));

    private static Brush Freeze(Brush b) { b.Freeze(); return b; }

    protected override void OnRender(DrawingContext dc)
    {
        var w = ActualWidth;
        var h = ActualHeight;
        dc.DrawRectangle(Back, null, new Rect(0, 0, w, h));
        var frac = Math.Clamp((Db + 60) / 60, 0, 1);
        if (frac <= 0) return;
        var brush = Db >= -1 ? Red : Db >= -6 ? Yellow : Green;
        dc.DrawRectangle(brush, null, new Rect(0, 0, w * frac, h));
    }
}
