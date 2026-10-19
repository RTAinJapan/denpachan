using System.IO;
using System.Runtime.InteropServices;

namespace Denpachan.Ui.Infrastructure;

public sealed record DriveFree(string Root, double FreeGb, double MinFreeGb);

public sealed record SystemSample(double CpuPercent, double MemoryFreeGb, double MemoryTotalGb, IReadOnlyList<DriveFree> Drives);

/// <summary>
/// 状態バーに出す PC の様子 (CPU 使用率、メモリの空き、録画先ドライブの空き)。
/// どれも OS の値を読むだけ (GetSystemTimes / GlobalMemoryStatusEx / GetDiskFreeSpaceEx) で数マイクロ秒。
/// 録画先がネットワークドライブのときに待たされることがあるので、呼ぶ側はスレッドプールで動かす。
/// </summary>
public sealed class SystemStats
{
    private long _lastIdle;
    private long _lastTotal;

    /// <summary>drives: 空きを見るフォルダと、その録画設定の空き容量の下限 (GB)</summary>
    public SystemSample Sample(IReadOnlyList<(string Dir, double MinFreeGb)> drives)
    {
        var cpu = SampleCpu();
        var (freeGb, totalGb) = SampleMemory();
        var byRoot = new Dictionary<string, DriveFree>(StringComparer.OrdinalIgnoreCase);
        foreach (var (dir, minFree) in drives)
        {
            string root;
            double free;
            try
            {
                root = Path.GetPathRoot(Path.GetFullPath(dir)) ?? dir;
                free = new DriveInfo(root).AvailableFreeSpace / (1024.0 * 1024 * 1024);
            }
            catch
            {
                continue;  // 外れたドライブや無効なパスは出さない
            }
            // 同じドライブを複数のチャンネルが使っていれば、下限は大きい方
            byRoot[root] = byRoot.TryGetValue(root, out var prev) ? prev with { MinFreeGb = Math.Max(prev.MinFreeGb, minFree) } : new DriveFree(root, free, minFree);
        }
        return new SystemSample(cpu, freeGb, totalGb, byRoot.Values.OrderBy(d => d.Root, StringComparer.OrdinalIgnoreCase).ToList());
    }

    // 前回からの idle / 全体の時間の差で出す (最初の 1 回は 0)
    private double SampleCpu()
    {
        if (!GetSystemTimes(out var idle, out var kernel, out var user)) return 0;
        var idleNow = ToLong(idle);
        var totalNow = ToLong(kernel) + ToLong(user);  // kernel には idle が含まれる
        var dIdle = idleNow - _lastIdle;
        var dTotal = totalNow - _lastTotal;
        var first = _lastTotal == 0;
        _lastIdle = idleNow;
        _lastTotal = totalNow;
        if (first || dTotal <= 0) return 0;
        return Math.Clamp(100.0 * (dTotal - dIdle) / dTotal, 0, 100);
    }

    private static (double FreeGb, double TotalGb) SampleMemory()
    {
        var status = new MEMORYSTATUSEX { dwLength = (uint)Marshal.SizeOf<MEMORYSTATUSEX>() };
        if (!GlobalMemoryStatusEx(ref status)) return (0, 0);
        const double gb = 1024.0 * 1024 * 1024;
        return (status.ullAvailPhys / gb, status.ullTotalPhys / gb);
    }

    private static long ToLong(System.Runtime.InteropServices.ComTypes.FILETIME t) =>
        ((long)t.dwHighDateTime << 32) | (uint)t.dwLowDateTime;

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetSystemTimes(out System.Runtime.InteropServices.ComTypes.FILETIME idle,
                                              out System.Runtime.InteropServices.ComTypes.FILETIME kernel,
                                              out System.Runtime.InteropServices.ComTypes.FILETIME user);

    [StructLayout(LayoutKind.Sequential)]
    private struct MEMORYSTATUSEX
    {
        public uint dwLength;
        public uint dwMemoryLoad;
        public ulong ullTotalPhys;
        public ulong ullAvailPhys;
        public ulong ullTotalPageFile;
        public ulong ullAvailPageFile;
        public ulong ullTotalVirtual;
        public ulong ullAvailVirtual;
        public ulong ullAvailExtendedVirtual;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GlobalMemoryStatusEx(ref MEMORYSTATUSEX buffer);
}

/// <summary>累積カウンタの「直近 N 秒の増分」。worker の再起動でカウンタが戻ったら数え直す。</summary>
public sealed class RateTracker
{
    private readonly TimeSpan _window;
    private readonly Queue<(DateTime Time, double Value)> _samples = new();

    public RateTracker(TimeSpan window) => _window = window;

    public double Add(double cumulative)
    {
        var now = DateTime.UtcNow;
        if (_samples.Count > 0 && cumulative < _samples.Last().Value) _samples.Clear();  // カウンタが戻った
        _samples.Enqueue((now, cumulative));
        while (_samples.Count > 1 && now - _samples.Peek().Time > _window) _samples.Dequeue();
        return cumulative - _samples.Peek().Value;
    }

    public void Reset() => _samples.Clear();
}
