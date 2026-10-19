using System.ComponentModel;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using System.Windows.Data;
using System.Windows.Markup;

namespace Denpachan.Ui.Infrastructure;

/// <summary>言語ファイルにある言語 (コードと、その言語での名前)</summary>
public sealed record LanguageInfo(string Code, string Name);

/// <summary>
/// 画面の文言。すべて言語ファイル (Lang/&lt;言語コード&gt;.json、exe に埋め込む) から引く。
///
/// - キーは「画面.場所.もの」 (例: "main.menu.file")。C++ (worker / engine) から届くメッセージは ID がそのままキー
///   ({"id":"net.dns","host":"x"}、src/common/include/denpachan/ui_message.h)。
/// - 文の中の {name} は引数で置き換える (Loc.F)。引数の名前は言語ファイルで並べ替えてよい。
/// - 今の言語に無いキーは日本語 (ja.json) の文を使う。それも無ければ [キー] を出す。
/// - 言語を変えると、XAML の {l:Tr キー} はその場で変わる。C# で組み立てている文は LanguageChanged で作り直す。
/// - 言語を足すときは Lang/ja.json を写して訳し、"_name" をその言語での名前にする (全体設定の一覧に出る)。
/// </summary>
public sealed partial class Loc : INotifyPropertyChanged
{
    public const string BaseLanguage = "ja";
    public const string Auto = "auto";

    public static Loc Instance { get; } = new();

    private readonly Dictionary<string, Dictionary<string, string>> _tables = [];
    private Dictionary<string, string> _current;
    private readonly Dictionary<string, string> _base;

    private Loc()
    {
        var asm = Assembly.GetExecutingAssembly();
        foreach (var name in asm.GetManifestResourceNames())
        {
            var m = ResourceName().Match(name);
            if (!m.Success) continue;
            using var stream = asm.GetManifestResourceStream(name)!;
            using var reader = new StreamReader(stream);
            _tables[m.Groups[1].Value] = Parse(reader.ReadToEnd());
        }
        _base = _tables.TryGetValue(BaseLanguage, out var b) ? b : [];
        _current = _base;
        Languages = _tables.Select(t => new LanguageInfo(t.Key, t.Value.TryGetValue("_name", out var n) ? n : t.Key))
            .OrderBy(l => l.Code == BaseLanguage ? 0 : 1).ThenBy(l => l.Code).ToList();
    }

    private static Dictionary<string, string> Parse(string json)
    {
        var table = new Dictionary<string, string>();
        if (JsonNode.Parse(json, documentOptions: new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip }) is JsonObject obj)
        {
            foreach (var (key, value) in obj)
            {
                if (value is JsonValue v && v.TryGetValue<string>(out var s)) table[key] = s;
            }
        }
        return table;
    }

    [GeneratedRegex(@"(?:^|\.)lang\.([A-Za-z-]+)\.json$")]
    private static partial Regex ResourceName();

    [GeneratedRegex(@"\{([A-Za-z0-9_]+)\}")]
    private static partial Regex Placeholder();

    /// <summary>言語ファイルがある言語 (日本語が先頭)</summary>
    public IReadOnlyList<LanguageInfo> Languages { get; }

    /// <summary>設定の値 ("auto" か言語コード)</summary>
    public string Setting { get; private set; } = Auto;

    /// <summary>実際に使っている言語のコード</summary>
    public string Language { get; private set; } = BaseLanguage;

    public event PropertyChangedEventHandler? PropertyChanged;

    /// <summary>言語が変わった (C# で組み立てている文を作り直す)</summary>
    public event Action? LanguageChanged;

    /// <summary>"auto" なら Windows の表示言語 (言語ファイルが無ければ日本語)</summary>
    public void SetLanguage(string? setting)
    {
        setting = string.IsNullOrEmpty(setting) ? Auto : setting;
        var code = setting == Auto ? Resolve(CultureInfo.CurrentUICulture) : _tables.ContainsKey(setting) ? setting : BaseLanguage;
        Setting = setting;
        if (code == Language) return;
        Language = code;
        _current = _tables.TryGetValue(code, out var t) ? t : _base;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(Binding.IndexerName));
        LanguageChanged?.Invoke();
    }

    private string Resolve(CultureInfo culture)
    {
        for (var c = culture; !string.IsNullOrEmpty(c.Name); c = c.Parent)
        {
            if (_tables.ContainsKey(c.Name)) return c.Name;
        }
        return BaseLanguage;
    }

    /// <summary>XAML の {l:Tr キー} から使う</summary>
    public string this[string key] => Get(key);

    public string Get(string key) =>
        _current.TryGetValue(key, out var s) || _base.TryGetValue(key, out s) ? s : $"[{key}]";

    /// <summary>文言</summary>
    public static string T(string key) => Instance.Get(key);

    /// <summary>{name} を引数で置き換えた文言。数値の書式は呼ぶ側で整えて文字列で渡す</summary>
    public static string F(string key, params (string Name, object? Value)[] args) => Format(Instance.Get(key), args);

    private static string Format(string text, IReadOnlyCollection<(string Name, object? Value)> args) =>
        args.Count == 0 ? text : Placeholder().Replace(text, m =>
        {
            foreach (var (name, value) in args)
            {
                if (name == m.Groups[1].Value) return Convert.ToString(value, CultureInfo.CurrentCulture) ?? "";
            }
            return m.Value;
        });

    /// <summary>
    /// C++ から届いたメッセージ ({"id": "net.dns", "host": "x"}) を文にする。null なら空。
    /// 言語ファイルに無い ID (engine の方が新しい) は ID と引数をそのまま出す。
    /// </summary>
    public static string Message(JsonNode? message)
    {
        if (message is not JsonObject obj || obj["id"] is not JsonValue idValue || !idValue.TryGetValue<string>(out var id)) return "";
        var args = obj.Where(p => p.Key != "id")
            .Select(p => (p.Key, (object?)(p.Value is JsonValue v && v.TryGetValue<string>(out var s) ? s : p.Value?.ToJsonString())))
            .ToList();
        if (id == "text") return args.FirstOrDefault(a => a.Key == "text").Item2 as string ?? "";
        var key = "msg." + id;
        if (Instance._current.ContainsKey(key) || Instance._base.ContainsKey(key)) return Format(Instance.Get(key), args);
        return args.Count == 0 ? id : $"{id} ({string.Join(", ", args.Select(a => $"{a.Key}={a.Item2}"))})";
    }
}

/// <summary>
/// XAML で文言を引く: Content="{l:Tr main.menu.file}"。言語を変えるとその場で変わる (Loc の索引子へのバインディング)。
/// </summary>
[MarkupExtensionReturnType(typeof(object))]
public sealed class TrExtension(string key) : MarkupExtension
{
    [ConstructorArgument("key")]
    public string Key { get; set; } = key;

    public override object ProvideValue(IServiceProvider serviceProvider) =>
        new Binding($"[{Key}]") { Source = Loc.Instance, Mode = BindingMode.OneWay }.ProvideValue(serviceProvider);
}
