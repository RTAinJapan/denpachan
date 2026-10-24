using System.Globalization;

namespace Denpachan.Ui.Infrastructure;

/// <summary>
/// 名前の自然順 (数字の部分は数として比べる。大文字小文字は区別しない)。
/// 音声デバイスの一覧で、DVS 4.5 の「DVS Transmit 1-2」「3-4」…「15-16」を番号の順に並べるため
/// (文字の順では 1-2、11-12、13-14、15-16、3-4… になる)。
/// </summary>
public sealed class NaturalOrder : IComparer<string>
{
    public static readonly NaturalOrder Instance = new();

    public int Compare(string? x, string? y)
    {
        if (ReferenceEquals(x, y)) return 0;
        if (x == null) return -1;
        if (y == null) return 1;
        int i = 0, j = 0;
        while (i < x.Length && j < y.Length)
        {
            if (char.IsAsciiDigit(x[i]) && char.IsAsciiDigit(y[j]))
            {
                // 数字の並び: 先頭の 0 を除いた桁数、次に数字の列で比べる (桁が多くても溢れない)
                var si = i;
                var sj = j;
                while (i < x.Length && char.IsAsciiDigit(x[i])) i++;
                while (j < y.Length && char.IsAsciiDigit(y[j])) j++;
                var a = x[si..i].TrimStart('0');
                var b = y[sj..j].TrimStart('0');
                var c = a.Length != b.Length ? a.Length.CompareTo(b.Length) : string.CompareOrdinal(a, b);
                if (c != 0) return c;
                continue;
            }
            // 数字以外の並び: 次の数字までを、大文字小文字を区別せずに比べる
            var ti = i;
            var tj = j;
            while (i < x.Length && !char.IsAsciiDigit(x[i])) i++;
            while (j < y.Length && !char.IsAsciiDigit(y[j])) j++;
            var t = string.Compare(x[ti..i], y[tj..j], CultureInfo.CurrentCulture, CompareOptions.IgnoreCase);
            if (t != 0) return t;
        }
        var rest = (x.Length - i).CompareTo(y.Length - j);
        return rest != 0 ? rest : string.Compare(x, y, StringComparison.OrdinalIgnoreCase);
    }
}
