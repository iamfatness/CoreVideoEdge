using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace CoreVideoEdge;

public sealed record EdgeCommandResult(bool Ok, string? Error);

// Runs corevideo-edge.exe as a child process and speaks its JSON-line control
// API (docs/PROTOCOL.md). The node owns all real work; this class only
// forwards operator commands and surfaces the node's state.
public sealed class EdgeNodeClient : IAsyncDisposable
{
    private readonly ConcurrentDictionary<string, TaskCompletionSource<EdgeCommandResult>> _pending = new();
    private readonly object _writeLock = new();
    private readonly string _logPath;
    private Process? _process;
    private StreamWriter? _log;
    private int _nextId;

    public EdgeNodeClient(string logPath) => _logPath = logPath;

    public event Action<EdgeState>? StateChanged;
    public event Action<string>? Exited;

    public EdgeState? Current { get; private set; }

    // `--fake` uses the synthetic meeting engine (COREVIDEO_EDGE_FAKE=1 or a
    // command-line flag on the app); otherwise the real Zoom engine beside it.
    public void Start(string nodeExecutable, string configPath, bool fakeEngine)
    {
        if (!File.Exists(nodeExecutable))
            throw new FileNotFoundException($"corevideo-edge.exe not found at {nodeExecutable}.");
        Directory.CreateDirectory(Path.GetDirectoryName(_logPath)!);
        RotateLog();
        _log = new StreamWriter(new FileStream(_logPath, FileMode.Append, FileAccess.Write, FileShare.ReadWrite), new UTF8Encoding(false))
        {
            AutoFlush = true,
        };
        var start = new ProcessStartInfo(nodeExecutable)
        {
            UseShellExecute = false,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
            StandardOutputEncoding = new UTF8Encoding(false),
            StandardErrorEncoding = new UTF8Encoding(false),
            WorkingDirectory = Path.GetDirectoryName(nodeExecutable)!,
        };
        start.ArgumentList.Add("--config");
        start.ArgumentList.Add(configPath);
        if (fakeEngine) start.ArgumentList.Add("--fake");
        _process = Process.Start(start) ?? throw new InvalidOperationException("Could not start corevideo-edge.exe.");
        _process.StandardInput.AutoFlush = true;
        _log.WriteLine($"--- {DateTime.Now:O} started {nodeExecutable} (fake={fakeEngine}) pid {_process.Id}");
        _ = Task.Run(() => ReadStdout(_process));
        _ = Task.Run(() => ReadStderr(_process));
    }

    public Task<EdgeCommandResult> SendAsync(string cmd, object? fields = null)
    {
        var process = _process;
        if (process is null || process.HasExited)
            return Task.FromResult(new EdgeCommandResult(false, "The Edge node is not running."));
        var id = Interlocked.Increment(ref _nextId).ToString();
        var node = fields is null ? new JsonObject() : JsonSerializer.SerializeToNode(fields)!.AsObject();
        node["cmd"] = cmd;
        node["id"] = id;
        var tcs = new TaskCompletionSource<EdgeCommandResult>(TaskCreationOptions.RunContinuationsAsynchronously);
        _pending[id] = tcs;
        try
        {
            lock (_writeLock) process.StandardInput.WriteLine(node.ToJsonString());
        }
        catch (Exception ex)
        {
            _pending.TryRemove(id, out _);
            return Task.FromResult(new EdgeCommandResult(false, $"Could not reach the Edge node: {ex.Message}"));
        }
        // Commands are answered within the node's loop; record-stop finalizes
        // files (bounded) and join starts the engine, so allow generous time.
        _ = Task.Delay(TimeSpan.FromSeconds(90)).ContinueWith(_ =>
        {
            if (_pending.TryRemove(id, out var late)) late.TrySetResult(new EdgeCommandResult(false, $"No answer from the Edge node to '{cmd}'."));
        });
        return tcs.Task;
    }

    private void ReadStdout(Process process)
    {
        try
        {
            string? line;
            while ((line = process.StandardOutput.ReadLine()) is not null)
            {
                if (line.Length == 0) continue;
                try
                {
                    using var doc = JsonDocument.Parse(line);
                    var root = doc.RootElement;
                    var type = root.TryGetProperty("type", out var t) ? t.GetString() : null;
                    if (type == "state")
                    {
                        var state = EdgeState.Parse(root);
                        Current = state;
                        StateChanged?.Invoke(state);
                    }
                    else if (type == "result" && root.TryGetProperty("id", out var idElement) &&
                             _pending.TryRemove(idElement.GetString() ?? "", out var tcs))
                    {
                        var ok = root.TryGetProperty("ok", out var okElement) && okElement.ValueKind == JsonValueKind.True;
                        var error = root.TryGetProperty("error", out var e) ? e.GetString() : null;
                        tcs.TrySetResult(new EdgeCommandResult(ok, error));
                    }
                }
                catch (JsonException)
                {
                    _log?.WriteLine($"[app] unparseable node output: {line}");
                }
            }
        }
        finally
        {
            foreach (var pending in _pending.Values) pending.TrySetResult(new EdgeCommandResult(false, "The Edge node exited."));
            _pending.Clear();
            var code = process.HasExited ? process.ExitCode.ToString() : "?";
            _log?.WriteLine($"--- {DateTime.Now:O} node exited ({code})");
            Exited?.Invoke($"The Edge node exited (code {code}). See {_logPath}.");
        }
    }

    private void ReadStderr(Process process)
    {
        string? line;
        while ((line = process.StandardError.ReadLine()) is not null) _log?.WriteLine(line);
    }

    private void RotateLog()
    {
        try
        {
            var info = new FileInfo(_logPath);
            if (info.Exists && info.Length > 8 * 1024 * 1024)
                File.Move(_logPath, _logPath + ".1", overwrite: true);
        }
        catch (IOException)
        {
            // A locked log is not a reason to refuse to start.
        }
    }

    public async ValueTask DisposeAsync()
    {
        var process = _process;
        _process = null;
        if (process is not null && !process.HasExited)
        {
            try
            {
                lock (_writeLock) process.StandardInput.WriteLine("{\"cmd\":\"quit\"}");
                process.StandardInput.Close();
                using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(20));
                await process.WaitForExitAsync(cts.Token).ConfigureAwait(false);
            }
            catch (Exception)
            {
                if (!process.HasExited) process.Kill(entireProcessTree: true);
            }
        }
        process?.Dispose();
        _log?.Dispose();
        _log = null;
    }
}
