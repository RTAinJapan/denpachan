using System.Text;

namespace Denpachan.Ui.Infrastructure;

/// <summary>録画ファイル名テンプレートの展開 (設定画面のプレビュー用。worker の filename.cpp と同じ規則)</summary>
public static class RecordFilename
{
    private static readonly string[] Reserved =
    [
        "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
        "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    ];

    public static string Expand(string template, int ch, string name, DateTime time, int seq, string codec, int width, int height,
                                string replacement)
    {
        var expanded = new StringBuilder();
        for (var i = 0; i < template.Length; i++)
        {
            if (template[i] != '{')
            {
                expanded.Append(template[i]);
                continue;
            }
            var close = template.IndexOf('}', i);
            if (close < 0)
            {
                expanded.Append(template, i, template.Length - i);
                break;
            }
            var key = template.Substring(i + 1, close - i - 1);
            expanded.Append(key switch
            {
                "ch" => ch.ToString(),
                "name" => name,
                "YYYY" => time.Year.ToString(),
                "YY" => (time.Year % 100).ToString("00"),
                "MM" => time.Month.ToString("00"),
                "DD" => time.Day.ToString("00"),
                "hh" => time.Hour.ToString("00"),
                "mm" => time.Minute.ToString("00"),
                "ss" => time.Second.ToString("00"),
                "seq" => seq.ToString(),
                "codec" => codec,
                "res" => width > 0 ? $"{width}x{height}" : "",
                _ => template.Substring(i, close - i + 1),
            });
            i = close;
        }

        var parts = expanded.ToString().Split('/', '\\')
            .Select(p => Sanitize(p, replacement))
            .Where(p => p.Length > 0)
            .ToList();
        return parts.Count == 0 ? $"ch{ch}" : string.Join('\\', parts);
    }

    private static string Sanitize(string part, string replacement)
    {
        var sb = new StringBuilder();
        foreach (var c in part)
        {
            if (c < 0x20 || ":*?\"<>|".Contains(c)) sb.Append(replacement);
            else sb.Append(c);
        }
        var s = sb.ToString().TrimEnd('.', ' ').TrimStart(' ');
        var stem = s.Split('.')[0].ToUpperInvariant();
        if (s is "." or ".." || Reserved.Contains(stem)) s = "_" + s;
        return s;
    }
}
