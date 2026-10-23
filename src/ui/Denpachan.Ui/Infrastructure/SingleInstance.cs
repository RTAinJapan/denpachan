using System.Diagnostics;
using System.Runtime.InteropServices;

namespace Denpachan.Ui.Infrastructure;

/// <summary>
/// 画面は 1 つだけ (engine の Local\denpachan_engine と同じ考え)。2 つ開くと、どちらも同じ engine に繋がり、
/// 片方を閉じて「受信を止める」を選ぶと、もう片方が開いたまま受信が止まる (そしてその画面が engine を起動し直す)。
/// 2 つ目は、既に開いている画面を前に出して終わる。画面の試験 (--fixture / --screenshot) は複数を続けて起動するので使わない
/// </summary>
public static class SingleInstance
{
    private static Mutex? _mutex;  // プロセスが終わるまで持つ (GC に回収させない)

    /// <summary>この画面が最初の 1 つなら true。2 つ目なら、既に開いている画面を前に出して false</summary>
    public static bool TryAcquire()
    {
        // 前の画面が落ちていればハンドルが閉じて Mutex は消えているので、起動し直せる
        _mutex = new Mutex(true, @"Local\denpachan_ui", out var createdNew);
        if (createdNew) return true;
        _mutex.Dispose();
        _mutex = null;
        BringExistingToFront();
        return false;
    }

    /// <summary>
    /// 閉じると決まったところで放す (engine を止めるのを待つ数秒の間に起動し直せるように。放さないと、その間の起動は
    /// 「開いている」とみなして終わり、画面が 1 つも無くなる)。取ったスレッド (UI スレッド) から呼ぶ
    /// </summary>
    public static void Release()
    {
        if (_mutex == null) return;
        try { _mutex.ReleaseMutex(); } catch (ApplicationException) { }
        _mutex.Dispose();
        _mutex = null;
    }

    private static void BringExistingToFront()
    {
        var me = Environment.ProcessId;
        foreach (var p in Process.GetProcessesByName("denpachan"))
        {
            try
            {
                if (p.Id == me) continue;
                var window = p.MainWindowHandle;
                if (window == IntPtr.Zero) continue;  // 起動の途中、または試験の画面
                if (IsIconic(window)) ShowWindow(window, SW_RESTORE);
                SetForegroundWindow(window);
            }
            catch (Exception)
            {
                // 見つけた画面が終わる途中などで取れなければ、前に出さずに終わるだけ
            }
            finally
            {
                p.Dispose();
            }
        }
    }

    private const int SW_RESTORE = 9;

    [DllImport("user32.dll")]
    private static extern bool IsIconic(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr hWnd);
}
