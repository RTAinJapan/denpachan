using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32;

namespace Denpachan.Ui.Infrastructure;

/// <summary>
/// 仮想カメラ (denpachan-vcam.dll、DirectShow) の登録。zip を展開してすぐ使えるように、
/// 画面の起動時に確かめて、登録されていない (またはフォルダを移して登録先がずれている) ときは
/// このユーザーだけに登録する (HKCU。管理者権限は要らない)。
/// </summary>
public static class VcamRegistration
{
    // src/vcam/guids.h の kCamClsid と同じ
    private static readonly string[] Clsids =
    [
        "{E952D5CE-ACFF-40F2-836E-000EAA95AC9E}",
        "{29E655A0-F1A2-48E5-9E45-2EEAFE2D401D}",
        "{5501670B-4CC8-42CC-97B3-6C03854B0905}",
        "{8A06D91E-DCE9-4E3E-A184-71AD2423AA35}",
    ];

    private const string VideoInputCategory = "{860BB310-5D01-11d0-BD3B-00A0C911CE86}";

    public static string DllPath => Path.Combine(AppContext.BaseDirectory, "denpachan-vcam.dll");

    /// <summary>4 台とも、この DLL で登録されているか (HKLM と HKCU を合わせた見え方で)</summary>
    public static bool IsRegistered()
    {
        foreach (var clsid in Clsids)
        {
            using var server = Registry.ClassesRoot.OpenSubKey($@"CLSID\{clsid}\InprocServer32");
            if (server?.GetValue(null) is not string path || !SamePath(path, DllPath)) return false;
            using var instance = Registry.ClassesRoot.OpenSubKey($@"CLSID\{VideoInputCategory}\Instance\{clsid}");
            if (instance == null) return false;
        }
        return true;
    }

    /// <summary>このユーザーに登録する。失敗したらその理由</summary>
    public static string? RegisterForUser() => CallDllInstall(true);

    /// <summary>このユーザーの登録を消す (マシン全体の登録は触らない)</summary>
    public static string? UnregisterForUser() => CallDllInstall(false);

    private static bool SamePath(string a, string b)
    {
        try
        {
            return string.Equals(Path.GetFullPath(a), Path.GetFullPath(b), StringComparison.OrdinalIgnoreCase);
        }
        catch (ArgumentException)
        {
            return false;
        }
    }

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int DllInstallFn(int install, [MarshalAs(UnmanagedType.LPWStr)] string cmdLine);

    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr LoadLibraryW(string path);

    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Ansi)]
    private static extern IntPtr GetProcAddress(IntPtr module, string name);

    [DllImport("kernel32")]
    private static extern bool FreeLibrary(IntPtr module);

    private static string? CallDllInstall(bool install)
    {
        if (!File.Exists(DllPath)) return $"{DllPath} がありません";
        var module = LoadLibraryW(DllPath);
        if (module == IntPtr.Zero) return $"{DllPath} を読み込めません (エラー {Marshal.GetLastWin32Error()})";
        try
        {
            var proc = GetProcAddress(module, "DllInstall");
            if (proc == IntPtr.Zero) return "DllInstall がありません";
            var hr = Marshal.GetDelegateForFunctionPointer<DllInstallFn>(proc)(install ? 1 : 0, "user");
            return hr < 0 ? $"登録に失敗しました (0x{hr:X8})" : null;
        }
        finally
        {
            FreeLibrary(module);
        }
    }
}
