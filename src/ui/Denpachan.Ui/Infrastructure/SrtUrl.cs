using System.Globalization;

namespace Denpachan.Ui.Infrastructure;

/// <summary>
/// SRT の URL (<c>srt://host:port?latency=200000&amp;streamid=...</c>)。OBS や MediaConnect からそのまま貼れるように、
/// 接続先の欄に URL を入れたら、ホスト・ポート・latency などに分ける (RTMP の URL の欄と同じく、頭から入れて動くように)。
/// URL の latency はマイクロ秒 (libsrt / FFmpeg の srt プロトコルと同じ)。
/// </summary>
public sealed record SrtUrl(string Host, int? Port, int? LatencyUs, string? StreamId, string? Passphrase, int? Pbkeylen)
{
    /// <summary>srt:// で始まっていれば分ける。そうでなければ null</summary>
    public static SrtUrl? Parse(string text)
    {
        var s = text.Trim();
        if (!s.StartsWith("srt://", StringComparison.OrdinalIgnoreCase)) return null;
        s = s["srt://".Length..];
        var query = "";
        if (s.IndexOf('?') is var q and >= 0)
        {
            query = s[(q + 1)..];
            s = s[..q];
        }
        s = s.TrimEnd('/');
        var host = s;
        int? port = null;
        // [IPv6]:port / host:port / :port (待ち受け)
        var colon = s.StartsWith('[') ? s.IndexOf("]:", StringComparison.Ordinal) + 1 : s.LastIndexOf(':');
        if (colon >= 0 && int.TryParse(s[(colon + 1)..], NumberStyles.None, CultureInfo.InvariantCulture, out var p))
        {
            host = s[..colon];
            port = p;
        }
        host = host.Trim('[', ']');
        int? latency = null, pbkeylen = null;
        string? streamId = null, passphrase = null;
        foreach (var pair in query.Split('&', StringSplitOptions.RemoveEmptyEntries))
        {
            var eq = pair.IndexOf('=');
            var key = (eq >= 0 ? pair[..eq] : pair).ToLowerInvariant();
            var value = eq >= 0 ? Uri.UnescapeDataString(pair[(eq + 1)..]) : "";
            switch (key)
            {
                case "latency":
                    if (int.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out var l)) latency = l;
                    break;
                case "streamid":
                    streamId = value;
                    break;
                case "passphrase":
                    passphrase = value;
                    break;
                case "pbkeylen":
                    if (int.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out var k)) pbkeylen = k;
                    break;
            }
        }
        // mode は見ない (送出側の URL の mode は相手から見た向きで、こちらの待ち受け / 接続とは逆のことがある)
        return new SrtUrl(host, port, latency, streamId, passphrase, pbkeylen);
    }

    /// <summary>設定の latency (ミリ秒) を、画面に出すマイクロ秒にする</summary>
    public static long UsFromMs(long ms) => ms * 1000;

    /// <summary>画面のマイクロ秒を、設定のミリ秒にする (libsrt はミリ秒で持つので、1 ms 未満は丸める)</summary>
    public static int MsFromUs(long us) => (int)Math.Clamp((us + 500) / 1000, 0, int.MaxValue);
}
