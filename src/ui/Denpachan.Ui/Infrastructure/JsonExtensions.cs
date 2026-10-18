using System.Text.Json.Nodes;

namespace Denpachan.Ui.Infrastructure;

/// <summary>engine から届く JSON を、欠けている項目があっても落ちずに読むための補助</summary>
public static class JsonExtensions
{
    public static JsonNode? At(this JsonNode? node, string key) => node is JsonObject o && o.TryGetPropertyValue(key, out var v) ? v : null;

    public static string Str(this JsonNode? node, string key, string def = "")
    {
        var v = node.At(key);
        return v is JsonValue jv && jv.TryGetValue<string>(out var s) ? s : def;
    }

    public static double Num(this JsonNode? node, string key, double def = 0)
    {
        var v = node.At(key);
        if (v is JsonValue jv)
        {
            if (jv.TryGetValue<double>(out var d)) return d;
            if (jv.TryGetValue<long>(out var l)) return l;
            if (jv.TryGetValue<int>(out var i)) return i;
        }
        return def;
    }

    public static int Int(this JsonNode? node, string key, int def = 0) => (int)Math.Round(node.Num(key, def));

    public static bool Bool(this JsonNode? node, string key, bool def = false)
    {
        var v = node.At(key);
        return v is JsonValue jv && jv.TryGetValue<bool>(out var b) ? b : def;
    }
}
