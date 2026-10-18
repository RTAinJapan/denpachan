using System.Diagnostics;
using System.IO;

namespace Denpachan.Ui.Infrastructure;

/// <summary>録画の CPU エンコードに使う外部 ffmpeg の確認 (worker と同じ順で探す)</summary>
public static class ExternalFfmpeg
{
    public sealed record CheckResult(bool Ok, string Message);

    public static string? Find(string configured)
    {
        if (configured.Length > 0) return File.Exists(configured) ? configured : null;
        var local = Path.Combine(AppContext.BaseDirectory, "ffmpeg.exe");
        if (File.Exists(local)) return local;
        foreach (var dir in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(';', StringSplitOptions.RemoveEmptyEntries))
        {
            try
            {
                var candidate = Path.Combine(dir.Trim().Trim('"'), "ffmpeg.exe");
                if (File.Exists(candidate)) return candidate;
            }
            catch (ArgumentException)
            {
                // PATH に変な文字が入っていても無視する
            }
        }
        return null;
    }

    public static async Task<CheckResult> CheckAsync(string configured)
    {
        var exe = Find(configured);
        if (exe == null)
        {
            return new(false, configured.Length > 0 ? $"見つかりません: {configured}" : "ffmpeg.exe が見つかりません (denpachan と同じフォルダにも PATH にもありません)");
        }
        try
        {
            var version = (await RunAsync(exe, "-hide_banner -version")).Split('\n').FirstOrDefault()?.Trim() ?? "";
            var encoders = await RunAsync(exe, "-hide_banner -encoders");
            string Mark(string name) => encoders.Contains($" {name} ") ? "○" : "×";
            var ok = encoders.Contains(" libx264 ");
            var message = $"{exe}\n{version}\nH.264 (libx264) {Mark("libx264")}   HEVC (libx265) {Mark("libx265")}   AV1 (libsvtav1) {Mark("libsvtav1")}";
            return new(ok, ok ? message : message + "\nこの ffmpeg には libx264 が入っていません");
        }
        catch (Exception e)
        {
            return new(false, $"{exe} を実行できません: {e.Message}");
        }
    }

    private static async Task<string> RunAsync(string exe, string args)
    {
        var psi = new ProcessStartInfo(exe, args)
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        using var p = Process.Start(psi) ?? throw new InvalidOperationException("起動できません");
        var output = p.StandardOutput.ReadToEndAsync();
        _ = p.StandardError.ReadToEndAsync();
        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        await p.WaitForExitAsync(cts.Token);
        return await output;
    }
}
