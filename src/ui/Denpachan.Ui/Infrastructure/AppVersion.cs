using System.Reflection;

namespace Denpachan.Ui.Infrastructure;

/// <summary>
/// 画面に出す版 (例: "0.1.0"、"0.2.0-rc.1")。版は CMakeLists.txt の project の VERSION、ラベルは build.ps1 -VersionLabel
/// (csproj が InformationalVersion にする)。SDK が後ろに付けるコミットのハッシュ (+abc123) は出さない。
/// </summary>
public static class AppVersion
{
    public static string Text { get; } = Read();

    private static string Read()
    {
        var asm = Assembly.GetExecutingAssembly();
        var info = asm.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion;
        if (!string.IsNullOrEmpty(info)) return info.Split('+')[0];
        var v = asm.GetName().Version;
        return v == null ? "" : $"{v.Major}.{v.Minor}.{v.Build}";
    }
}
