using System.Collections.ObjectModel;
using System.Windows.Input;

namespace CoreVideoEdge.Desktop.ViewModels;

public sealed class IsoRowViewModel : ObservableObject
{
    public IsoRowViewModel(int slot) => Slot = slot;

    public int Slot { get; }
    public string SlotTag => $"S{Slot}";
    public string Name { get; private set; } = "";
    public string FileName { get; private set; } = "";
    public string FilePath { get; private set; } = "";
    public string Duration { get; private set; } = "";
    public string Frames { get; private set; } = "";
    public string Lost { get; private set; } = "";
    public bool LostNonZero { get; private set; }
    public string Held { get; private set; } = "";
    public string Cadence { get; private set; } = "";
    public double? AudioDb { get; private set; }
    public bool MeterVisible => AudioDb is not null;
    public string Size { get; private set; } = "";
    public string Tag { get; private set; } = "";
    public Tone TagTone { get; private set; }
    public bool Failed { get; private set; }
    public string? Warning { get; private set; }
    public bool HasWarning => !string.IsNullOrEmpty(Warning);
    public string AccessibleName { get; private set; } = "";

    public void ApplyArmed(EdgeSubscription sub, EdgeParticipant? p)
    {
        Name = sub.Name;
        FileName = FilePath = Duration = Frames = Lost = Held = Cadence = Size = "";
        LostNonZero = false;
        AudioDb = p?.AudioDb;
        Tag = "ARMED";
        TagTone = Tone.Accent;
        Failed = false;
        Warning = sub.Present ? null : "Left the meeting. Start is refused while an ISO guest is away.";
        AccessibleName = $"{SlotTag}, {Name}, ISO armed";
        RaiseAll();
    }

    public void ApplyStream(EdgeIsoStream s, EdgeParticipant? p)
    {
        Name = s.Name;
        FilePath = s.Path;
        FileName = Path.GetFileName(s.Path);
        // Duration: seconds of audio actually written (48 kHz), a measured count.
        Duration = Format.Clock((s.AudioSamples + s.SilenceSamples) * 1000 / 48000);
        Frames = Format.Count(s.VideoFrames);
        var lost = s.VideoLost + (s.VideoDropped ?? 0);
        Lost = Format.Count(lost);
        LostNonZero = lost > 0;
        Held = s.HeldRepeats is long held && held > 0 ? $"held {Format.Count(held)}, source stalled" : s.HeldRepeats is not null ? "0" : "";
        Cadence = s.CadenceRepeats is long rep && s.CadenceSkipped is long skip ? $"+{Format.Count(rep)} / −{Format.Count(skip)}" : "";
        AudioDb = s.State is "recording" or "opening" ? p?.AudioDb : null;
        Size = s.Bytes is long b ? Format.Bytes(b) : "";
        (Tag, TagTone) = s.State switch
        {
            "opening" => ("OPENING", Tone.Warn),
            "recording" => ("REC", Tone.Rec),
            "finalizing" => ("FINALIZING", Tone.Warn),
            "finalized" => ("SAVED", Tone.Live),
            "failed" => ("FAILED", Tone.Error),
            _ => (s.State.ToUpperInvariant(), Tone.Neutral),
        };
        Failed = s.State == "failed";
        Warning = s.Warning;
        AccessibleName = $"{SlotTag}, {Name}, {Tag.ToLowerInvariant()}, {Frames} frames, lost {Lost}" + (HasWarning ? $", {Warning}" : "");
        RaiseAll();
    }

    private void RaiseAll()
    {
        foreach (var n in new[]
                 {
                     nameof(Name), nameof(FileName), nameof(FilePath), nameof(Duration), nameof(Frames), nameof(Lost),
                     nameof(LostNonZero), nameof(Held), nameof(Cadence), nameof(AudioDb), nameof(MeterVisible), nameof(Size),
                     nameof(Tag), nameof(TagTone), nameof(Failed), nameof(Warning), nameof(HasWarning), nameof(AccessibleName),
                 })
            Raise(n);
    }
}

// Record: folder, format, start/stop and the per-ISO table. Brief section 4.4.
public sealed class RecordViewModel : ObservableObject
{
    private readonly MainViewModel _main;
    private string _prefix = "";
    private string? _folderError;
    private string? _formatError;
    private string? _actionError;
    private (int Height, int Fps)? _pendingFormat;
    private bool _starting;
    private bool _stopping;
    private bool _confirmStop;
    private int _startingCount;

    public RecordViewModel(MainViewModel main)
    {
        _main = main;
        ChangeFolderCommand = new AsyncCommand(ChangeFolderAsync, main.ReportError, () => !Locked);
        OpenFolderCommand = new RelayCommand(() =>
        {
            var dir = S?.SessionDir ?? S?.RecordFolder;
            if (!string.IsNullOrEmpty(dir)) _main.Shell.OpenFolder(dir);
        });
        SetHeightCommand = new AsyncCommand(p => SetFormatAsync(Convert.ToInt32(p), null), main.ReportError, _ => !Locked);
        SetFpsCommand = new AsyncCommand(p => SetFormatAsync(null, Convert.ToInt32(p)), main.ReportError, _ => !Locked);
        PrimaryCommand = new AsyncCommand(PrimaryAsync, main.ReportError, () => PrimaryEnabled);
        ConfirmStopCommand = new AsyncCommand(StopAsync, main.ReportError);
        CancelStopCommand = new RelayCommand(() => { _confirmStop = false; RaiseAll(); });
        SavePrefixCommand = new AsyncCommand(SavePrefixAsync, main.ReportError, () => !Locked);
    }

    public AsyncCommand ChangeFolderCommand { get; }
    public ICommand OpenFolderCommand { get; }
    public AsyncCommand SetHeightCommand { get; }
    public AsyncCommand SetFpsCommand { get; }
    public AsyncCommand PrimaryCommand { get; }
    public AsyncCommand ConfirmStopCommand { get; }
    public ICommand CancelStopCommand { get; }
    public AsyncCommand SavePrefixCommand { get; }
    public ObservableCollection<IsoRowViewModel> Rows { get; } = new();

    // Persists folder/prefix to edge.json; set by the app (null in tests/screenshots).
    public Action<string?, string?>? SaveSettings { get; set; }

    private EdgeState? S => _main.State;
    public bool Recording => S?.Recording == true;
    public bool Locked => Recording || _starting || _stopping;

    // ------------------------------------------------------------ folder

    public string Folder => S?.RecordFolder ?? "";
    public bool HasFolder => Folder.Length > 0;
    public string FolderShort => HasFolder ? Format.MiddleTruncate(Folder, 56) : "No folder chosen";
    public string FreeText
    {
        get
        {
            if (!HasFolder || _main.FreeBytes is not long free) return "";
            var vol = _main.Shell.VolumeName(Folder);
            return $"{Format.Bytes(free)} free{(vol is null ? "" : $" on {vol}")}";
        }
    }
    public Tone FreeTone => _main.DiskTone;
    public string? FolderError => _folderError ?? (HasFolder && !Directory.Exists(Folder) && !Recording
        ? "This folder does not exist. The node creates it at start, or choose another folder." : null);
    public bool FolderErrorVisible => FolderError is not null;

    public string Prefix { get => _prefix; set => Set(ref _prefix, value); }

    private async Task ChangeFolderAsync()
    {
        var path = await _main.Shell.PickFolderAsync(HasFolder ? Folder : null);
        if (string.IsNullOrEmpty(path)) return;
        var result = await _main.SendAsync("set-record-folder", new { path });
        _folderError = result.Ok ? null : result.Error;
        if (result.Ok) SaveSettings?.Invoke(path, null);
        RaiseAll();
    }

    private async Task SavePrefixAsync()
    {
        var prefix = _prefix.Trim();
        if (prefix == S?.FilePrefix) return;
        var result = await _main.SendAsync("set-file-prefix", new { prefix });
        _folderError = result.Ok ? null : result.Error;
        if (result.Ok) SaveSettings?.Invoke(null, prefix);
        RaiseAll();
    }

    // ------------------------------------------------------------ format

    public int Height => _pendingFormat?.Height ?? S?.RecordFormat?.Height ?? 0;
    public int Fps => _pendingFormat?.Fps ?? S?.RecordFormat?.Fps ?? -1;
    public bool FormatKnown => S?.RecordFormat is not null;
    public bool Is1080 => Height == 1080;
    public bool Is720 => Height == 720;
    public bool FpsSource => Fps == 0;
    public bool Fps30 => Fps == 30;
    public bool Fps60 => Fps == 60;
    public bool FormatPending => _pendingFormat is not null;
    public string? FormatError => _formatError;
    public bool FormatErrorVisible => _formatError is not null;
    public string LockedNote => Recording ? "Locked while recording" : "";

    private async Task SetFormatAsync(int? height, int? fps)
    {
        var current = S?.RecordFormat;
        var h = height ?? current?.Height ?? 1080;
        var f = fps ?? current?.Fps ?? 0;
        if (current is not null && current.Height == h && current.Fps == f) return;
        _formatError = null;
        _pendingFormat = (h, f);
        RaiseAll();
        var result = await _main.SendAsync("set-record-format", new { height = h, fps = f });
        if (!result.Ok)
        {
            _formatError = result.Error;
            _pendingFormat = null;
        }
        RaiseAll();
    }

    // ------------------------------------------------------------ start / stop

    public int ArmedCount => S?.Subscriptions.Count(x => x.Iso) ?? 0;

    // Why Start is refused, stated before the operator tries.
    public string? StartBlockedReason
    {
        get
        {
            var s = S;
            if (s is null) return "The Edge node is not running.";
            if (s.Recording) return null;
            if (!s.InMeeting) return "Join a meeting first.";
            if (s.EngineOn == false) return "Engine is off. Turn it on in Connect.";
            if (s.RawMediaActive != true) return "Zoom raw media is not active yet. The host has to allow this node to record.";
            if (ArmedCount == 0) return "No ISO sources armed. Arm participants in the ISO band on Routing.";
            if (!HasFolder) return "Choose a record folder.";
            if (s.Subscriptions.Any(x => x.Iso && !x.Present)) return "An armed ISO guest has left the meeting. Disarm them or wait for them.";
            return null;
        }
    }

    public bool PrimaryEnabled => !_starting && !_stopping && (Recording || StartBlockedReason is null);
    public bool ConfirmStopVisible => _confirmStop && Recording;
    public bool Busy => _starting || _stopping;

    public string PrimaryText
    {
        get
        {
            if (_starting) return $"Starting {Format.Plural(_startingCount, "ISO file", "ISO files")}…";
            if (_stopping)
            {
                var total = S?.Streams.Count ?? 0;
                var closed = S?.Streams.Count(x => x.State is "finalized" or "failed") ?? 0;
                return total > 0 ? $"Finalizing files… {closed} of {total} closed" : "Finalizing files…";
            }
            return Recording ? "Stop recording" : "Start recording";
        }
    }
    public string PrimaryTimer => Recording && S?.RecordingElapsedMs is long ms ? Format.Clock(ms) : "";
    public string? ActionError => _actionError ?? (Recording ? null : StartBlockedReason);
    public bool ActionErrorIsFailure => _actionError is not null;
    public bool ActionErrorVisible => ActionError is not null;
    public string? RecordingWarning => S?.RecordingWarning;
    public bool RecordingWarningVisible => !string.IsNullOrEmpty(RecordingWarning);

    private Task PrimaryAsync()
    {
        if (Recording)
        {
            _confirmStop = true;  // stopping closes every file: confirm inline
            RaiseAll();
            return Task.CompletedTask;
        }
        return StartAsync();
    }

    public async Task StartAsync()
    {
        _actionError = null;
        _starting = true;
        _startingCount = ArmedCount;
        RaiseAll();
        try
        {
            var result = await _main.SendAsync("record-start");
            if (!result.Ok) _actionError = result.Error;
        }
        finally
        {
            _starting = false;
            RaiseAll();
        }
    }

    public async Task StopAsync()
    {
        _confirmStop = false;
        _actionError = null;
        _stopping = true;
        RaiseAll();
        try
        {
            var result = await _main.SendAsync("record-stop");
            if (!result.Ok) _actionError = result.Error;
        }
        finally
        {
            _stopping = false;
            RaiseAll();
        }
    }

    // ------------------------------------------------------------ table

    public bool TableVisible => Rows.Count > 0;
    public bool EmptyVisible => Rows.Count == 0;
    public string TableTitle => Recording ? "ISO files" : S?.Streams.Count > 0 ? "Last take" : "Armed for the next take";

    public void Apply(EdgeState s)
    {
        if (_pendingFormat is { } pf && s.RecordFormat is { } rf && rf.Height == pf.Height && rf.Fps == pf.Fps) _pendingFormat = null;
        if (!_prefixEditing) _prefix = s.FilePrefix;
        if (!s.Recording) _confirmStop = false;

        // Live or last-take streams win; otherwise the armed list for the next take.
        var wanted = new List<(int Slot, Action<IsoRowViewModel> Apply)>();
        if (s.Streams.Count > 0)
        {
            foreach (var st in s.Streams.OrderBy(x => x.Slot))
            {
                var p = s.Participants?.FirstOrDefault(x => x.Id == st.ParticipantId);
                wanted.Add((st.Slot, r => r.ApplyStream(st, p)));
            }
        }
        if (!s.Recording && s.Streams.Count == 0)
        {
            foreach (var sub in s.Subscriptions.Where(x => x.Iso).OrderBy(x => x.Slot))
            {
                var p = s.Participants?.FirstOrDefault(x => x.Id == sub.ParticipantId);
                wanted.Add((sub.Slot, r => r.ApplyArmed(sub, p)));
            }
        }
        for (var i = Rows.Count - 1; i >= 0; --i)
            if (wanted.All(w => w.Slot != Rows[i].Slot)) Rows.RemoveAt(i);
        for (var index = 0; index < wanted.Count; ++index)
        {
            var (slot, apply) = wanted[index];
            var row = Rows.FirstOrDefault(r => r.Slot == slot);
            if (row is null)
            {
                row = new IsoRowViewModel(slot);
                Rows.Insert(Math.Min(index, Rows.Count), row);
            }
            apply(row);
        }
        RaiseAll();
    }

    private bool _prefixEditing;
    public void SetPrefixEditing(bool editing) => _prefixEditing = editing;

    public void Tick() => RaiseAll();

    private void RaiseAll()
    {
        foreach (var n in Names) Raise(n);
        ChangeFolderCommand.NotifyCanExecuteChanged();
        SetHeightCommand.NotifyCanExecuteChanged();
        SetFpsCommand.NotifyCanExecuteChanged();
        PrimaryCommand.NotifyCanExecuteChanged();
        SavePrefixCommand.NotifyCanExecuteChanged();
    }

    private static readonly string[] Names =
    [
        nameof(Recording), nameof(Locked), nameof(Folder), nameof(HasFolder), nameof(FolderShort), nameof(FreeText), nameof(FreeTone),
        nameof(FolderError), nameof(FolderErrorVisible), nameof(Prefix), nameof(Height), nameof(Fps), nameof(FormatKnown),
        nameof(Is1080), nameof(Is720), nameof(FpsSource), nameof(Fps30), nameof(Fps60), nameof(FormatPending), nameof(FormatError),
        nameof(FormatErrorVisible), nameof(LockedNote), nameof(ArmedCount), nameof(StartBlockedReason), nameof(PrimaryEnabled),
        nameof(ConfirmStopVisible), nameof(Busy), nameof(PrimaryText), nameof(PrimaryTimer), nameof(ActionError),
        nameof(ActionErrorIsFailure), nameof(ActionErrorVisible), nameof(RecordingWarning), nameof(RecordingWarningVisible),
        nameof(TableVisible), nameof(EmptyVisible), nameof(TableTitle),
    ];
}
