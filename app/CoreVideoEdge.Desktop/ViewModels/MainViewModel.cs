using System.Windows.Input;
using CoreVideoEdge.Desktop.Services;

namespace CoreVideoEdge.Desktop.ViewModels;

public enum Screen { Connect, Routing, Monitor, Record, Comms }

// Tone of a pill, tag or line. Colour is never the only signal: every tone is
// shown with a word, and warn/error also carry an icon.
public enum Tone { Neutral, Accent, Live, Warn, Error, Rec }

public sealed record BannerItem(string Problem, string Recovery, string? ActionText = null, ICommand? Action = null);

public sealed class AppOptions
{
    public string? LogPath { get; init; }
    // Join is allowed without signing in only when the node can join another
    // way: the fake engine, or one of the documented token variables is set.
    public bool JoinWithoutSignIn { get; init; }
}

// The shell: owns the node state, the header status cluster, the failure banner
// and the five screens. Everything the operator sees is derived from the last
// node state line (measured) or from the OS (disk free), never assumed.
public sealed class MainViewModel : ObservableObject
{
    public static readonly TimeSpan StaleAfter = TimeSpan.FromSeconds(2);
    public static readonly TimeSpan LostAfter = TimeSpan.FromSeconds(10);
    private const long DiskWarnBytes = 20L * 1024 * 1024 * 1024;
    private const long DiskErrorBytes = 5L * 1024 * 1024 * 1024;

    private readonly INodeChannel _node;
    private readonly AppOptions _options;
    private Screen _screen = Screen.Connect;
    private EdgeState? _state;
    private TimeSpan? _lastStateAt;
    private string? _nodeExited;
    private readonly List<(TimeSpan At, long Free)> _diskSamples = new();
    private string? _toast;
    private string? _appError;
    private TimeSpan _toastUntil;

    public MainViewModel(INodeChannel node, IAccountService? account, IShellServices shell, IClock clock, AppOptions? options = null)
    {
        _node = node;
        Shell = shell;
        Clock = clock;
        _options = options ?? new AppOptions();
        Connect = new ConnectViewModel(this, account, _options);
        Routing = new RoutingViewModel(this);
        Monitor = new MonitorViewModel(this);
        Record = new RecordViewModel(this);
        Comms = new CommsViewModel(this);
        SelectScreenCommand = new RelayCommand(p =>
        {
            if (p is Screen s) SelectedScreen = s;
            else if (p is string name && Enum.TryParse<Screen>(name, out var parsed)) SelectedScreen = parsed;
        });
        RetryCommand = new RelayCommand(() => _ = RetryAsync());
        GoToConnectCommand = new RelayCommand(() => SelectedScreen = Screen.Connect);
        GoToRoutingCommand = new RelayCommand(() => SelectedScreen = Screen.Routing);
        OpenLogFolderCommand = new RelayCommand(() =>
        {
            var dir = _options.LogPath is null ? null : Path.GetDirectoryName(_options.LogPath);
            if (dir is not null) Shell.OpenFolder(dir);
        });
    }

    public IShellServices Shell { get; }
    public IClock Clock { get; }
    public ConnectViewModel Connect { get; }
    public RoutingViewModel Routing { get; }
    public MonitorViewModel Monitor { get; }
    public RecordViewModel Record { get; }
    public CommsViewModel Comms { get; }
    public ICommand SelectScreenCommand { get; }
    public ICommand RetryCommand { get; }
    public ICommand GoToConnectCommand { get; }
    public ICommand GoToRoutingCommand { get; }
    public ICommand OpenLogFolderCommand { get; }

    public EdgeState? State => _state;

    public Screen SelectedScreen
    {
        get => _screen;
        set
        {
            if (!Set(ref _screen, value)) return;
            Raise(nameof(IsConnect));
            Raise(nameof(IsRouting));
            Raise(nameof(IsMonitor));
            Raise(nameof(IsRecord));
            Raise(nameof(IsComms));
            if (value != Screen.Comms) _ = Comms.ReleaseAllAsync();
        }
    }

    public bool IsConnect => _screen == Screen.Connect;
    public bool IsRouting => _screen == Screen.Routing;
    public bool IsMonitor => _screen == Screen.Monitor;
    public bool IsRecord => _screen == Screen.Record;
    public bool IsComms => _screen == Screen.Comms;

    // ------------------------------------------------------------ node I/O

    // Every operator action goes through here, so tests can see exactly what
    // JSON each action sends.
    public Task<EdgeCommandResult> SendAsync(string cmd, object? fields = null) => _node.SendAsync(cmd, fields);

    public void Apply(EdgeState state)
    {
        var previous = _state;
        _state = state;
        _lastStateAt = Clock.Now;
        if (previous?.Recording == true && !state.Recording && state.Streams.Count > 0)
        {
            var saved = state.Streams.Count(s => s.State == "finalized");
            ShowToast(saved == state.Streams.Count
                ? $"Recording saved, {Format.Plural(saved, "file", "files")}"
                : $"Recording stopped: {saved} of {state.Streams.Count} files saved");
        }
        Connect.Apply(state);
        Routing.Apply(state);
        Monitor.Apply(state);
        Record.Apply(state);
        Comms.Apply(state);
        Refresh();
    }

    public void NodeExited(string message)
    {
        _nodeExited = message;
        Refresh();
    }

    // Called a few times a second: elapsed counters, staleness, disk.
    public void Tick()
    {
        SampleDisk();
        Connect.Tick();
        Routing.Tick();
        Record.Tick();
        Comms.Tick();
        if (_toast is not null && Clock.Now > _toastUntil) Toast = null;
        Refresh();
    }

    public void ShowToast(string text)
    {
        _toastUntil = Clock.Now + TimeSpan.FromSeconds(4);
        Toast = text;
    }

    public string? Toast
    {
        get => _toast;
        private set { if (Set(ref _toast, value)) Raise(nameof(ToastVisible)); }
    }

    public bool ToastVisible => _toast is not null;

    // ------------------------------------------------------------ staleness

    public TimeSpan? StateAge => _lastStateAt is TimeSpan at ? Clock.Now - at : null;
    public bool IsStale => StateAge > StaleAfter;
    public bool IsLost => _nodeExited is not null || StateAge > LostAfter;
    public string StaleText => StateAge is TimeSpan age && age > StaleAfter ? $"stale {(int)age.TotalSeconds}s" : "";

    // ------------------------------------------------------------ header

    public bool HeaderValuesVisible => _state is not null && !IsLost;

    // After 10 s without a state line the screens' values are removed and the
    // reason is shown instead (brief: stale data).
    public bool ScreensVisible => !IsLost;
    public string LostText => _nodeExited ?? (StateAge is TimeSpan age ? $"The Edge node has not answered for {(int)age.TotalSeconds} s." : "");

    // Narrow windows (< 1200 px): the wordmark gives way to the mark, and the
    // disk pill only shows when it is warning.
    private bool _compact;
    public bool CompactHeader { get => _compact; set { if (Set(ref _compact, value)) Refresh(); } }
    public bool WordmarkVisible => !_compact;
    public bool DiskPillVisible => DiskVisible && (!_compact || DiskTone != Tone.Neutral);

    // The mark's satellite tile is the only part that carries state (brief 6B):
    // red while any ISO records, amber while something is degraded, cyan otherwise.
    public Tone MarkTone => _state?.Recording == true ? Tone.Rec
        : BannerVisible || _state?.MovingRooms == true ? Tone.Warn
        : Tone.Accent;
    public bool FakeEngine => _state?.EngineKind == "fake";

    public bool MeetingPillVisible => HeaderValuesVisible && _state!.MeetingId is not null && !_state.CanJoin;
    public string MeetingPillText => _state?.MeetingId is string id ? $"Meeting {Format.MeetingId(id)}" : "";
    public string MeetingPillName => _state?.MeetingId is string id
        ? $"Meeting {id}, {PhaseWords(_state.Phase)}{(_state.MovingRooms ? ", moving rooms" : "")}"
        : "";
    public Tone MeetingTone => _state?.MovingRooms == true ? Tone.Warn : _state?.InMeeting == true ? Tone.Live : Tone.Neutral;
    public string MeetingWord => _state is null ? "" : _state.MovingRooms ? "moving rooms" : PhaseWords(_state.Phase);

    public bool EnginePillVisible => HeaderValuesVisible && _state!.EngineOn is not null;
    public string EngineWord => Connect.EngineVisual switch
    {
        EngineVisual.Off => "Off",
        EngineVisual.Standby => "Armed",
        EngineVisual.Requesting => "Requesting",
        EngineVisual.On => "On",
        EngineVisual.Moving => "Moving",
        EngineVisual.Failed => "Refused",
        _ => "",
    };
    public Tone EngineTone => Connect.EngineVisual switch
    {
        EngineVisual.On => Tone.Live,
        EngineVisual.Requesting or EngineVisual.Moving => Tone.Warn,
        EngineVisual.Failed => Tone.Error,
        _ => Tone.Neutral,
    };
    public string EnginePillName => $"Engine {EngineWord}";

    public int Cap => _state?.MaxSubscriptions ?? 0;
    public int RoutedCount => _state?.Subscriptions.Count ?? 0;
    public int IsoCount => _state?.Subscriptions.Count(s => s.Iso) ?? 0;
    public bool CapVisible => HeaderValuesVisible && Cap > 0;
    public string RoutedText => $"{RoutedCount}/{Cap}";
    public string IsoText => $"{IsoCount}/{Cap}";
    public string RoutedName => $"Routed {RoutedCount} of {Cap}";
    public string IsoName => $"ISO {IsoCount} of {Cap}";

    public bool RecVisible => HeaderValuesVisible && _state!.Recording;
    public string RecTimer => _state?.RecordingElapsedMs is long ms ? Format.Clock(ms) : "";
    public string RecName => _state?.RecordingElapsedMs is long ms ? $"Recording, {Format.Clock(ms)} elapsed" : "Recording";

    // Disk free (measured by the OS) on the record folder's volume.
    public long? FreeBytes { get; private set; }
    public double? DiskBytesPerSecond { get; private set; }  // measured from free-space change while recording
    public bool DiskVisible => HeaderValuesVisible && FreeBytes is not null;
    public string DiskText => FreeBytes is long b ? $"{Format.Bytes(b)} free" : "";
    public Tone DiskTone
    {
        get
        {
            if (FreeBytes is not long free) return Tone.Neutral;
            if (free < DiskErrorBytes) return Tone.Error;
            if (DiskBytesPerSecond is double rate && rate > 0 && free / rate < 600) return Tone.Error;
            return free < DiskWarnBytes ? Tone.Warn : Tone.Neutral;
        }
    }
    public string DiskName => FreeBytes is long b
        ? $"Disk {Format.Bytes(b)} free{(DiskTone == Tone.Error ? ", critically low" : DiskTone == Tone.Warn ? ", low" : "")}"
        : "";

    private void SampleDisk()
    {
        var folder = _state?.RecordFolder;
        if (string.IsNullOrWhiteSpace(folder))
        {
            FreeBytes = null;
            DiskBytesPerSecond = null;
            _diskSamples.Clear();
            return;
        }
        var free = Shell.FreeBytes(folder);
        FreeBytes = free;
        if (free is null || _state?.Recording != true)
        {
            _diskSamples.Clear();
            DiskBytesPerSecond = null;
            return;
        }
        var now = Clock.Now;
        _diskSamples.Add((now, free.Value));
        _diskSamples.RemoveAll(s => now - s.At > TimeSpan.FromSeconds(10));
        var first = _diskSamples[0];
        var span = (now - first.At).TotalSeconds;
        // Only a measured rate over at least 5 s; never a guess.
        DiskBytesPerSecond = span >= 5 ? Math.Max(0, (first.Free - free.Value) / span) : null;
    }

    // ------------------------------------------------------------ banner

    public IReadOnlyList<BannerItem> BannerItems { get; private set; } = [];
    public BannerItem? Banner => BannerItems.Count > 0 ? BannerItems[0] : null;
    public bool BannerVisible => BannerItems.Count > 0;
    public string BannerMore => BannerItems.Count > 1 ? $"+{BannerItems.Count - 1} more" : "";
    public string BannerName => Banner is BannerItem b ? $"Failure: {b.Problem} {b.Recovery}" : "";

    private IReadOnlyList<BannerItem> BuildBanner()
    {
        var items = new List<BannerItem>();
        if (_nodeExited is not null)
        {
            items.Add(new BannerItem(_nodeExited, "Restart CoreVideo Edge. The node log has the reason.", "Open log folder", OpenLogFolderCommand));
            return items;
        }
        if (_appError is not null)
            items.Add(new BannerItem(_appError, "Fix the cause and try again.", "Dismiss", new RelayCommand(ClearError)));
        if (_state is null) return items;
        if (StateAge > LostAfter)
            items.Add(new BannerItem($"The Edge node has not answered for {(int)StateAge!.Value.TotalSeconds} s.",
                "Values are hidden until it answers. If it does not, restart CoreVideo Edge.", "Open log folder", OpenLogFolderCommand));
        var s = _state;
        if (s.ConfigErrors.Count > 0)
            items.Add(new BannerItem($"edge.json: {string.Join("; ", s.ConfigErrors)}",
                $"Fix {(s.ConfigPath ?? "the node config")} and restart CoreVideo Edge."));
        if (s.Phase == "failed" && s.Error is not null)
            items.Add(new BannerItem(s.Error, "Fix the cause, then join again.", "Retry join", RetryCommand));
        else if (s.Phase == "idle" && s.Error is not null && Connect.HasJoinedThisSession)
            items.Add(new BannerItem(s.Error, "Join again from Connect when the meeting is running.", "Go to Connect", GoToConnectCommand));
        if (s.EngineSilentMs is long silent)
            items.Add(new BannerItem($"The Zoom engine has been silent for {silent / 1000} s.",
                "If it stays silent, leave and join again."));
        if (s.Recording && s.RecordingWarning is not null)
            items.Add(new BannerItem(s.RecordingWarning, "The take is still running. Fix the cause or stop recording."));
        var failed = s.Streams.Where(x => x.State == "failed").ToList();
        if (failed.Count > 0)
            items.Add(new BannerItem($"ISO failed for {string.Join(", ", failed.Select(f => f.Name))}: {failed[0].Warning ?? "the writer stopped"}.",
                "See Record for the reason. Other ISOs keep recording."));
        if (DiskTone == Tone.Error && FreeBytes is long free)
            items.Add(new BannerItem($"Only {Format.Bytes(free)} free on the record disk.",
                s.Recording ? "Free space now or stop recording." : "Choose a folder on another disk before recording."));
        return items;
    }

    public async Task RetryAsync()
    {
        if (_state?.Phase == "failed")
        {
            SelectedScreen = Screen.Connect;
            await Connect.JoinAsync(_state.CanEndOtherSession && Connect.LastJoinEndedOther);
        }
    }

    public void ReportError(Exception ex) => ReportError(ex.Message);

    // An action that threw (sign-in, folder save...). Loud: a banner with the
    // message until the operator dismisses it or the action succeeds.
    public void ReportError(string message)
    {
        _appError = message;
        Refresh();
    }

    public void ClearError()
    {
        if (_appError is null) return;
        _appError = null;
        Refresh();
    }

    // ------------------------------------------------------------ plumbing

    public static string PhaseWords(string phase) => phase switch
    {
        "idle" => "not in a meeting",
        "starting-engine" => "starting the Zoom engine",
        "authenticating" => "authenticating",
        "joining" => "joining",
        "in-meeting" => "in meeting",
        "leaving" => "leaving",
        "failed" => "join failed",
        _ => phase,
    };

    private void Refresh()
    {
        BannerItems = BuildBanner();
        foreach (var name in RefreshedProperties) Raise(name);
    }

    private static readonly string[] RefreshedProperties =
    [
        nameof(State), nameof(MarkTone), nameof(ScreensVisible), nameof(LostText), nameof(WordmarkVisible), nameof(DiskPillVisible),
        nameof(IsStale), nameof(IsLost), nameof(StaleText), nameof(HeaderValuesVisible), nameof(FakeEngine),
        nameof(MeetingPillVisible), nameof(MeetingPillText), nameof(MeetingPillName), nameof(MeetingTone), nameof(MeetingWord),
        nameof(EnginePillVisible), nameof(EngineWord), nameof(EngineTone), nameof(EnginePillName),
        nameof(Cap), nameof(RoutedCount), nameof(IsoCount), nameof(CapVisible), nameof(RoutedText), nameof(IsoText),
        nameof(RoutedName), nameof(IsoName), nameof(RecVisible), nameof(RecTimer), nameof(RecName),
        nameof(FreeBytes), nameof(DiskBytesPerSecond), nameof(DiskVisible), nameof(DiskText), nameof(DiskTone), nameof(DiskName),
        nameof(BannerItems), nameof(Banner), nameof(BannerVisible), nameof(BannerMore), nameof(BannerName),
    ];
}
