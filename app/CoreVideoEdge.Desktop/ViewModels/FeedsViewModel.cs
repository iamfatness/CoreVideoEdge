using System.Globalization;
using System.Windows.Input;

namespace CoreVideoEdge.Desktop.ViewModels;

// "Send to Pro" (Routing screen): live feeds of every routed slot to CoreVideo
// Pro over NDI or SRT (docs/PROTOCOL.md set-feeds / "Feeds state"). The SRT
// passphrase is write-only: typed here, sent once, never shown back.
public sealed class FeedsViewModel : ObservableObject
{
    private readonly MainViewModel _main;
    private string? _selected;          // segment the operator picked (before the node confirms)
    private string? _pendingMode;
    private string _host = "";
    private string _port = "10000";
    private string _latency = "120";
    private string _passphrase = "";
    private string? _error;
    private bool _fieldsFromState;

    public FeedsViewModel(MainViewModel main)
    {
        _main = main;
        SelectOffCommand = new AsyncCommand(() => SelectAsync("off"), main.ReportError, () => !Busy);
        SelectNdiCommand = new AsyncCommand(() => SelectAsync("ndi"), main.ReportError, () => !Busy);
        SelectSrtCommand = new AsyncCommand(() => SelectAsync("srt"), main.ReportError, () => !Busy);
        ApplySrtCommand = new AsyncCommand(() => SendSrtAsync(clearPassphrase: false), main.ReportError, () => !Busy && SrtHostValid);
        ClearPassphraseCommand = new AsyncCommand(() => SendSrtAsync(clearPassphrase: true), main.ReportError, () => !Busy);
    }

    public AsyncCommand SelectOffCommand { get; }
    public AsyncCommand SelectNdiCommand { get; }
    public AsyncCommand SelectSrtCommand { get; }
    public AsyncCommand ApplySrtCommand { get; }
    public AsyncCommand ClearPassphraseCommand { get; }

    private EdgeFeeds? F => _main.State?.Feeds;

    // The block only exists when the node publishes feeds (it supports them).
    public bool Visible => F is not null;
    public string NodeMode => F?.Mode ?? "off";
    public string Shown => _selected ?? NodeMode;
    public bool IsOff => Shown == "off";
    public bool IsNdi => Shown == "ndi";
    public bool IsSrt => Shown == "srt";
    public bool Busy => _pendingMode is not null;

    public string Host { get => _host; set { if (Set(ref _host, value)) RaiseAll(); } }
    public string Port { get => _port; set { if (Set(ref _port, value)) RaiseAll(); } }
    public string Latency { get => _latency; set { if (Set(ref _latency, value)) RaiseAll(); } }
    public string Passphrase { get => _passphrase; set { if (Set(ref _passphrase, value)) RaiseAll(); } }

    public bool SrtHostValid => _host.Trim().Length > 0 && int.TryParse(_port, out _) && int.TryParse(_latency, out _);
    public bool PassphraseSet => F?.SrtEncrypted == true;
    public string PassphraseNote => PassphraseSet
        ? "A passphrase is set (it is never shown). Type a new one to replace it."
        : "No passphrase: the feed is not encrypted.";
    public string PortsNote => int.TryParse(_port, out var p) && _main.Cap > 0
        ? $"Slot n sends to port {p} + n − 1 (S1 {p} … S{_main.Cap} {p + _main.Cap - 1}), Pro's SRT input listeners."
        : "Slot n sends to base port + n − 1.";
    public string ApplySrtText => NodeMode == "srt" ? "Update SRT" : "Send over SRT";

    // The node's refusal, verbatim, shown at the fields.
    public string? Error => _error;
    public bool ErrorVisible => _error is not null;

    // One line of measured health; per-slot detail lives on the Monitor tiles.
    public string Summary
    {
        get
        {
            var f = F;
            if (f is null || f.Mode == "off") return "Feeds off. Pro gets these guests only from the ISO files.";
            var kind = f.Mode == "ndi" ? "NDI" : "SRT";
            if (f.Slots.Count == 0) return $"{kind} on. Route a guest to send it.";
            var sending = f.Slots.Count(s => s.State == "sending");
            var failed = f.Slots.Count(s => s.State == "failed");
            return $"{kind}: {sending} of {f.Slots.Count} sending" + (failed > 0 ? $", {failed} failed" : "");
        }
    }
    public Tone SummaryTone
    {
        get
        {
            var f = F;
            if (f is null || f.Mode == "off" || f.Slots.Count == 0) return Tone.Neutral;
            if (f.Slots.Any(s => s.State == "failed")) return Tone.Error;
            return f.Slots.All(s => s.State == "sending") ? Tone.Live : Tone.Warn;
        }
    }
    public string? FirstError => F?.Slots.FirstOrDefault(s => s.Error is not null)?.Error;
    public bool FirstErrorVisible => FirstError is not null;

    private async Task SelectAsync(string mode)
    {
        _error = null;
        _selected = mode;
        RaiseAll();
        // SRT needs an address first: picking the segment only reveals the fields.
        if (mode == "srt" && NodeMode != "srt") return;
        if (mode == NodeMode) { _selected = null; RaiseAll(); return; }
        await SendAsync(new { mode });
    }

    private Task SendSrtAsync(bool clearPassphrase)
    {
        var port = int.Parse(_port.Trim(), CultureInfo.InvariantCulture);
        var latency = int.Parse(_latency.Trim(), CultureInfo.InvariantCulture);
        var host = _host.Trim();
        if (clearPassphrase)
            return SendAsync(new { mode = "srt", srtHost = host, srtBasePort = port, srtLatencyMs = latency, srtPassphrase = "" });
        // Omitted = the node keeps the passphrase it has.
        return _passphrase.Length > 0
            ? SendAsync(new { mode = "srt", srtHost = host, srtBasePort = port, srtLatencyMs = latency, srtPassphrase = _passphrase })
            : SendAsync(new { mode = "srt", srtHost = host, srtBasePort = port, srtLatencyMs = latency });
    }

    private async Task SendAsync(object fields)
    {
        _pendingMode = (string)fields.GetType().GetProperty("mode")!.GetValue(fields)!;
        RaiseAll();
        var result = await _main.SendAsync("set-feeds", fields);
        if (result.Ok)
        {
            _passphrase = "";        // write-only: drop it from the app as soon as it is sent
            _error = null;
            if (F?.Mode == _pendingMode) _selected = null;
        }
        else
        {
            _error = result.Error;
        }
        _pendingMode = null;
        RaiseAll();
    }

    public void Apply(EdgeState s)
    {
        var f = s.Feeds;
        if (f is not null)
        {
            if (_selected == f.Mode) _selected = null;
            // Fill the fields once from what the node holds, then leave them to the operator.
            if (!_fieldsFromState && f.SrtHost is not null)
            {
                _host = f.SrtHost;
                if (f.SrtBasePort is int p) _port = p.ToString(CultureInfo.InvariantCulture);
                if (f.SrtLatencyMs is int l) _latency = l.ToString(CultureInfo.InvariantCulture);
                _fieldsFromState = true;
            }
        }
        RaiseAll();
    }

    private void RaiseAll()
    {
        foreach (var n in Names) Raise(n);
        SelectOffCommand.NotifyCanExecuteChanged();
        SelectNdiCommand.NotifyCanExecuteChanged();
        SelectSrtCommand.NotifyCanExecuteChanged();
        ApplySrtCommand.NotifyCanExecuteChanged();
        ClearPassphraseCommand.NotifyCanExecuteChanged();
    }

    private static readonly string[] Names =
    [
        nameof(Visible), nameof(NodeMode), nameof(Shown), nameof(IsOff), nameof(IsNdi), nameof(IsSrt), nameof(Busy),
        nameof(Host), nameof(Port), nameof(Latency), nameof(Passphrase), nameof(SrtHostValid), nameof(PassphraseSet),
        nameof(PassphraseNote), nameof(PortsNote), nameof(ApplySrtText), nameof(Error), nameof(ErrorVisible),
        nameof(Summary), nameof(SummaryTone), nameof(FirstError), nameof(FirstErrorVisible),
    ];
}
