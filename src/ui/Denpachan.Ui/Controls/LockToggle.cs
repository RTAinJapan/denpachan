using System.Globalization;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace Denpachan.Ui.Controls;

/// <summary>
/// 操作ロックの鍵アイコン。クリックでロック (確認なし)、ロック中は 1 秒長押しで解除。
/// 長押しの間は、鍵の色が右から左へ灰色になっていき、左端まで灰色になったら解除する (開いた鍵になる)。
/// 途中で離す・外に出ると解除しない。
/// </summary>
public sealed class LockToggle : FrameworkElement
{
    public static readonly DependencyProperty IsLockedProperty = DependencyProperty.Register(
        nameof(IsLocked), typeof(bool), typeof(LockToggle),
        new FrameworkPropertyMetadata(false, FrameworkPropertyMetadataOptions.AffectsRender | FrameworkPropertyMetadataOptions.BindsTwoWayByDefault));
    /// <summary>長押しの進み具合 (0 = 色のまま、1 = 全部灰色)</summary>
    private static readonly DependencyProperty HoldProgressProperty = DependencyProperty.Register(
        "HoldProgress", typeof(double), typeof(LockToggle), new FrameworkPropertyMetadata(0.0, FrameworkPropertyMetadataOptions.AffectsRender));

    public bool IsLocked { get => (bool)GetValue(IsLockedProperty); set => SetValue(IsLockedProperty, value); }

    private static readonly TimeSpan HoldTime = TimeSpan.FromSeconds(1);
    private const string LockGlyph = "";    // Lock
    private const string UnlockGlyph = "";  // Unlock

    private static readonly Brush LockedBrush = Freeze(new SolidColorBrush(Color.FromRgb(0xF0, 0xB4, 0x4C)));
    private static readonly Brush MonoBrush = Freeze(new SolidColorBrush(Color.FromRgb(0x9A, 0x9C, 0xA3)));
    private static readonly Brush HoverBrush = Freeze(new SolidColorBrush(Color.FromRgb(0x34, 0x37, 0x3D)));
    private static readonly Typeface IconFace = new(new FontFamily("Segoe Fluent Icons, Segoe MDL2 Assets"),
                                                    FontStyles.Normal, FontWeights.Normal, FontStretches.Normal);

    private static Brush Freeze(Brush b) { b.Freeze(); return b; }

    private bool _pressed;
    private bool _unlockedByHold;  // この押下で解除した (離したときにロックし直さない)
    private AnimationClock? _hold;

    public LockToggle()
    {
        Width = 26;
        Height = 26;
        Cursor = Cursors.Hand;
        Stylus.SetIsPressAndHoldEnabled(this, false);  // タッチの長押しを右クリックにしない
    }

    protected override void OnMouseLeftButtonDown(MouseButtonEventArgs e)
    {
        e.Handled = true;
        if (!CaptureMouse()) return;
        _pressed = true;
        _unlockedByHold = false;
        if (IsLocked) StartHold();
    }

    protected override void OnMouseLeftButtonUp(MouseButtonEventArgs e)
    {
        if (!_pressed) return;
        e.Handled = true;
        var lockNow = !IsLocked && !_unlockedByHold && IsInside(e);
        EndPress();
        if (lockNow) IsLocked = true;
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        if (_pressed && !IsInside(e)) CancelHold();
    }

    protected override void OnLostMouseCapture(MouseEventArgs e) => EndPress();

    protected override void OnMouseEnter(MouseEventArgs e) => InvalidateVisual();
    protected override void OnMouseLeave(MouseEventArgs e) => InvalidateVisual();

    private bool IsInside(MouseEventArgs e) => new Rect(RenderSize).Contains(e.GetPosition(this));

    private void EndPress()
    {
        if (!_pressed) return;
        _pressed = false;
        CancelHold();
        ReleaseMouseCapture();
    }

    private void StartHold()
    {
        var clock = new DoubleAnimation(0, 1, HoldTime).CreateClock();
        clock.Completed += (_, _) =>
        {
            if (_hold != clock) return;  // 途中でやめた
            CancelHold();
            _unlockedByHold = true;
            IsLocked = false;
        };
        _hold = clock;
        ApplyAnimationClock(HoldProgressProperty, clock);
    }

    private void CancelHold()
    {
        if (_hold == null) return;
        _hold = null;
        ApplyAnimationClock(HoldProgressProperty, null);  // 色に戻す
    }

    protected override void OnRender(DrawingContext dc)
    {
        var w = ActualWidth;
        var h = ActualHeight;
        dc.DrawRoundedRectangle(IsMouseOver ? HoverBrush : Brushes.Transparent, null, new Rect(0, 0, w, h), 4, 4);  // 透明でもマウス操作を受ける

        var locked = IsLocked;
        var text = new FormattedText(locked ? LockGlyph : UnlockGlyph, CultureInfo.InvariantCulture, FlowDirection.LeftToRight,
                                     IconFace, Math.Min(w, h) * 0.62, MonoBrush, VisualTreeHelper.GetDpi(this).PixelsPerDip);
        var icon = text.BuildGeometry(new Point((w - text.Width) / 2, (h - text.Height) / 2));
        if (!locked)
        {
            dc.DrawGeometry(MonoBrush, null, icon);
            return;
        }

        // 鍵の形の範囲で、右から (進み具合の分だけ) 灰色にする
        var b = icon.Bounds;
        var split = b.Left + b.Width * (1 - (double)GetValue(HoldProgressProperty));
        dc.PushClip(new RectangleGeometry(new Rect(0, 0, Math.Max(0, split), h)));
        dc.DrawGeometry(LockedBrush, null, icon);
        dc.Pop();
        dc.PushClip(new RectangleGeometry(new Rect(split, 0, Math.Max(0, w - split), h)));
        dc.DrawGeometry(MonoBrush, null, icon);
        dc.Pop();
    }
}
