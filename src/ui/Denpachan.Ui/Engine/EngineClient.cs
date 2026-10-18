using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Text;
using System.Text.Json.Nodes;

namespace Denpachan.Ui.Engine;

public sealed class EngineException(string message) : Exception(message);

/// <summary>
/// engine との IPC (名前付きパイプ \\.\pipe\denpachan、改行区切りの JSON)。設計書 §10.2。
/// 切れたら 1 秒ごとに繋ぎ直す。engine が動いていなければ起動する。
/// イベントは裏のスレッドから上がるので、画面の更新は呼び出し側で UI スレッドに戻すこと。
/// </summary>
public sealed class EngineClient : IDisposable
{
    private const string PipeName = "denpachan";
    private static readonly TimeSpan CallTimeout = TimeSpan.FromSeconds(15);

    private readonly CancellationTokenSource _cts = new();
    private readonly ConcurrentDictionary<int, TaskCompletionSource<JsonNode?>> _pending = new();
    private readonly SemaphoreSlim _writeLock = new(1, 1);
    private StreamWriter? _writer;
    private int _nextId;
    private bool _launchedEngine;

    public event Action<JsonNode>? StateReceived;
    public event Action<bool>? ConnectionChanged;

    public bool IsConnected { get; private set; }

    public void Start() => _ = Task.Run(() => RunAsync(_cts.Token));

    public void Dispose()
    {
        _cts.Cancel();
        _writeLock.Dispose();
    }

    private async Task RunAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            try
            {
                using var pipe = new NamedPipeClientStream(".", PipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
                await pipe.ConnectAsync(1000, ct);
                using var reader = new StreamReader(pipe, new UTF8Encoding(false));
                _writer = new StreamWriter(pipe, new UTF8Encoding(false)) { AutoFlush = true, NewLine = "\n" };
                SetConnected(true);
                while (!ct.IsCancellationRequested)
                {
                    var line = await reader.ReadLineAsync(ct);
                    if (line == null) break;  // engine が終わった
                    Dispatch(line);
                }
            }
            catch (TimeoutException)
            {
                // engine が動いていない: 一度だけ起動を試みる
                if (!_launchedEngine)
                {
                    _launchedEngine = true;
                    EngineLauncher.EnsureRunning();
                }
            }
            catch (OperationCanceledException) when (ct.IsCancellationRequested)
            {
                break;
            }
            catch (Exception e)
            {
                Debug.WriteLine($"engine connection: {e.Message}");
            }
            _writer = null;
            SetConnected(false);
            foreach (var p in _pending) p.Value.TrySetException(new EngineException("engine との接続が切れました"));
            _pending.Clear();
            try { await Task.Delay(1000, ct); } catch (OperationCanceledException) { break; }
        }
    }

    private void SetConnected(bool connected)
    {
        if (IsConnected == connected) return;
        IsConnected = connected;
        if (connected) _launchedEngine = false;  // 次に切れたらまた起動を試みる
        ConnectionChanged?.Invoke(connected);
    }

    private void Dispatch(string line)
    {
        JsonNode? msg;
        try { msg = JsonNode.Parse(line); } catch { return; }
        if (msg is not JsonObject obj) return;

        if (obj.TryGetPropertyValue("event", out var ev) && ev?.GetValue<string>() == "state")
        {
            if (obj["data"] is JsonNode data) StateReceived?.Invoke(data);
            return;
        }
        if (obj["id"] is JsonValue idValue && idValue.TryGetValue<int>(out var id) && _pending.TryRemove(id, out var tcs))
        {
            if (obj["error"] is JsonNode err) tcs.TrySetException(new EngineException(err.GetValue<string>()));
            else tcs.TrySetResult(obj["result"]?.DeepClone());
        }
    }

    /// <summary>要求を送り、応答を待つ。エラーは EngineException。</summary>
    public async Task<JsonNode?> CallAsync(string method, JsonObject? parameters = null)
    {
        var writer = _writer ?? throw new EngineException("engine に接続していません");
        var id = Interlocked.Increment(ref _nextId);
        var tcs = new TaskCompletionSource<JsonNode?>(TaskCreationOptions.RunContinuationsAsynchronously);
        _pending[id] = tcs;
        var request = new JsonObject { ["id"] = id, ["method"] = method, ["params"] = parameters ?? new JsonObject() };
        await _writeLock.WaitAsync();
        try
        {
            await writer.WriteLineAsync(request.ToJsonString());
        }
        catch (Exception e)
        {
            _pending.TryRemove(id, out _);
            throw new EngineException($"送信できませんでした: {e.Message}");
        }
        finally
        {
            _writeLock.Release();
        }
        var done = await Task.WhenAny(tcs.Task, Task.Delay(CallTimeout));
        if (done != tcs.Task)
        {
            _pending.TryRemove(id, out _);
            throw new EngineException("engine が応答しません");
        }
        return await tcs.Task;
    }
}

/// <summary>engine (denpachan-engine.exe) が動いていなければ起動する。UI を閉じても engine は動き続ける。</summary>
public static class EngineLauncher
{
    public static string EngineDirectory => AppContext.BaseDirectory;

    public static void EnsureRunning()
    {
        if (Process.GetProcessesByName("denpachan-engine").Length > 0) return;
        var exe = Path.Combine(EngineDirectory, "denpachan-engine.exe");
        if (!File.Exists(exe)) return;
        try
        {
            Process.Start(new ProcessStartInfo(exe)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                WorkingDirectory = EngineDirectory,
            });
        }
        catch (Exception e)
        {
            Debug.WriteLine($"cannot start engine: {e.Message}");
        }
    }
}
