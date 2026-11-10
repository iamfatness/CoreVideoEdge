using System.Collections.ObjectModel;
using System.Windows.Input;

namespace CoreVideoEdge.Desktop.ViewModels;

// One talk key: a person (or a group channel). The cell IS the key (ZComms
// model). Every state word comes from the node's comms state; nothing here
// shows "on air" before the engine reports the channel keyed.
public sealed class CommsCellViewModel : ObservableObject
{
    private string _name = "";
    private string _stateText = "";
    private bool _hot, _armed, _ready, _off;
    private string? _keyHint;

    public CommsCellViewModel(int slot) { Slot = slot; }

    public int Slot { get; }
    public bool Held { get; set; }

    public string Name { get => _name; set { if (Set(ref _name, value)) Raise(nameof(AutomationName)); } }
    public string StateText { get => _stateText; set { if (Set(ref _stateText, value)) Raise(nameof(AutomationName)); } }
    public bool Hot { get => _hot; set => Set(ref _hot, value); }       // keyed and someone is listening
    public bool Armed { get => _armed; set => Set(ref _armed, value); } // keyed, nobody in the channel
    public bool Ready { get => _ready; set => Set(ref _ready, value); }
    public bool Off { get => _off; set { if (Set(ref _off, value)) Raise(nameof(Enabled)); } }   // unreachable from this room
    public bool Enabled => !_off;
    public string? KeyHint { get => _keyHint; set { if (Set(ref _keyHint, value)) Raise(nameof(KeyHintVisible)); } }
    public bool KeyHintVisible => _keyHint is not null;
    public string AutomationName => $"Talk to {Name}: {StateText}";
}

public sealed class CommsChipViewModel
{
    public required int Slot { get; init; }
    public required string Text { get; init; }
    public required bool On { get; init; }
    public required bool Busy { get; init; }
    public required string Tip { get; init; }
    public required ICommand Command { get; init; }
}

public sealed class CommsPersonViewModel
{
    public required string Name { get; init; }
    public required bool Talkback { get; init; }
    public bool NoTalkback => !Talkback;
    public required IReadOnlyList<CommsChipViewModel> Chips { get; init; }
}

public sealed class CommsMeterSegment : ObservableObject
{
    private bool _lit;
    public required Tone Tone { get; init; }
    public bool Lit { get => _lit; set => Set(ref _lit, value); }
}

public sealed class CommsViewModel : ObservableObject
{
    public const int MeterSegments = 12;
    private const string WebClientWhy = "On the Zoom web client. Ask them to rejoin in the Zoom desktop app.";

    private readonly MainViewModel _main;
    private EdgeState? _state;
    private string _cellSig = "";
    private string _peopleSig = "";
    private bool _latchMode, _editMode, _settingsOpen, _opsOpen, _allHeld;
    private string? _error;

    public CommsViewModel(MainViewModel main)
    {
        _main = main;
        PressCommand = new RelayCommand(p => { if (p is CommsCellViewModel c) _ = PressAsync(c); });
        ReleaseCommand = new RelayCommand(p => { if (p is CommsCellViewModel c) _ = ReleaseAsync(c); });
        AllPressCommand = new RelayCommand(() => _ = AllPressAsync());
        AllReleaseCommand = new RelayCommand(() => _ = AllReleaseAsync());
        ToggleLatchModeCommand = new RelayCommand(() => LatchMode = !LatchMode);
        ToggleEditCommand = new RelayCommand(() => EditMode = !EditMode);
        ToggleSettingsCommand = new RelayCommand(() => SettingsOpen = !SettingsOpen);
        ToggleOpsCommand = new RelayCommand(() => OpsOpen = !OpsOpen);
        LeaveCommand = new RelayCommand(() => _ = SendAsync("leave"));
        SidetoneCommand = new RelayCommand(() => _ = SendAsync("comms-sidetone", new { on = !(Comms?.Mic.Sidetone ?? false) }));
        ToneCommand = new RelayCommand(() => _ = SendAsync("comms-tone", new { on = !(Comms?.Mic.Tone ?? false) }));
        GainDownCommand = new RelayCommand(() => _ = SendGainAsync(-1));
        GainUpCommand = new RelayCommand(() => _ = SendGainAsync(+1));
        for (var i = 0; i < MeterSegments; i++)
            Meter.Add(new CommsMeterSegment { Tone = i == MeterSegments - 1 ? Tone.Rec : i >= 8 ? Tone.Warn : Tone.Live });
    }

    public ObservableCollection<CommsCellViewModel> Cells { get; } = new();
    public ObservableCollection<CommsPersonViewModel> People { get; } = new();
    public ObservableCollection<CommsMeterSegment> Meter { get; } = new();

    public ICommand PressCommand { get; }
    public ICommand ReleaseCommand { get; }
    public ICommand AllPressCommand { get; }
    public ICommand AllReleaseCommand { get; }
    public ICommand ToggleLatchModeCommand { get; }
    public ICommand ToggleEditCommand { get; }
    public ICommand ToggleSettingsCommand { get; }
    public ICommand ToggleOpsCommand { get; }
    public ICommand LeaveCommand { get; }
    public ICommand SidetoneCommand { get; }
    public ICommand ToneCommand { get; }
    public ICommand GainDownCommand { get; }
    public ICommand GainUpCommand { get; }

    private EdgeComms? Comms => _state?.Comms;

    // Tests await the last send.
    public Task LastSend { get; private set; } = Task.CompletedTask;

    // ------------------------------------------------------------ screen states

    public bool InMeeting => _state?.InMeeting == true;
    public bool NotInMeeting => !InMeeting;
    public bool Waiting => InMeeting && Comms is null;
    public bool Unavailable => InMeeting && Comms is { Available: false };
    public bool DeskVisible => InMeeting && Comms is { Available: true };
    public string UnavailableText => Comms?.Reason ?? "Talkback is not available.";
    public bool HostHintVisible => Unavailable && (Comms?.Reason ?? "").Contains("host", StringComparison.OrdinalIgnoreCase);

    public bool GridVisible => DeskVisible && !_editMode && Cells.Count > 0;
    public bool EditVisible => DeskVisible && _editMode;
    public bool EmptyDeskVisible => DeskVisible && !_editMode && Cells.Count == 0;
    public string EmptyDeskText => Comms?.Roster.Count > 0 ? "In the meeting. Channels forming…" : "In the meeting. Waiting for participants…";

    // ------------------------------------------------------------ rail

    public string RailText
    {
        get
        {
            if (_state is null || !InMeeting) return "Not in a meeting";
            var room = Comms?.Room is string r ? $" · Room {r}" : "";
            return _state.MeetingId is string id ? $"Meeting {Format.MeetingId(id)}{room}" : $"In meeting{room}";
        }
    }
    // Lamps are machine truth even when talkback is refused (LINK can be up
    // while CHANNEL never comes up), so they follow the report, not the desk.
    private bool Reported => InMeeting && Comms is not null;
    public bool LampLink => Reported && Comms!.LampLink;
    public bool LampMic => Reported && Comms!.LampMeetingMic;
    public bool LampChannel => Reported && Comms!.LampChannel;
    public bool LampTx => Reported && Comms!.LampTx;

    // ------------------------------------------------------------ all call

    public bool AllHot => DeskVisible && Comms!.Channels.Count > 0 && Comms.Channels.All(c => c.Keyed) && Comms.Channels.Any(c => c.Keyed && c.Listeners > 0);
    public bool AllArmed => DeskVisible && Comms!.Channels.Count > 0 && Comms.Channels.All(c => c.Keyed) && !Comms.Channels.Any(c => c.Listeners > 0);
    public string AllText => AllHot ? (Comms!.Channels.All(c => c.Latched) ? "ALL CALL · ON AIR · LATCHED" : "ALL CALL · ON AIR") : "ALL CALL";

    // ------------------------------------------------------------ strip

    public bool LatchMode { get => _latchMode; set => Set(ref _latchMode, value); }
    public bool EditMode
    {
        get => _editMode;
        set { if (Set(ref _editMode, value)) RaiseDesk(); }
    }
    public bool SettingsOpen { get => _settingsOpen; set => Set(ref _settingsOpen, value); }
    public bool OpsOpen { get => _opsOpen; set { if (Set(ref _opsOpen, value)) RaiseOps(); } }

    public string? Error { get => _error; private set { if (Set(ref _error, value)) Raise(nameof(ErrorVisible)); } }
    public bool ErrorVisible => _error is not null;

    // ------------------------------------------------------------ settings

    public IReadOnlyList<string> Mics => Comms?.Mic.Devices ?? [];
    public IReadOnlyList<string> Outputs => Comms?.Mic.SidetoneDevices ?? [];
    private bool _applying;
    public string? SelectedMic
    {
        get => Comms?.Mic.Device;
        set { if (!_applying && value is not null && value != Comms?.Mic.Device) _ = SendAsync("comms-mic", new { device = value }); }
    }
    public string? SelectedOutput
    {
        get => Comms?.Mic.SidetoneDevice;
        set { if (!_applying && value is not null && value != Comms?.Mic.SidetoneDevice) _ = SendAsync("comms-sidetone-out", new { device = value }); }
    }
    public bool SidetoneOn => Comms?.Mic.Sidetone == true;
    public bool ToneOn => Comms?.Mic.Tone == true;
    public string GainText => Comms is { } c ? $"{(c.Mic.GainDb > 0 ? "+" : "")}{c.Mic.GainDb} dB" : "";
    public string FramesSent => Comms?.FramesSent.ToString("N0") ?? "";
    public string ChannelSends => Comms?.ChannelSends.ToString("N0") ?? "";
    public string Underruns => Comms?.Underruns.ToString("N0") ?? "";
    public string SendErrors => Comms?.SendErrors.ToString("N0") ?? "";
    public bool SendErrorsWarn => Comms?.SendErrors > 0;
    public double? MicDb => DeskVisible ? Comms!.Mic.PeakDbfs : null;
    public string MicLevelName => MicDb is double db ? $"Microphone level {db:0} dBFS" : "Microphone level not measured";

    // ------------------------------------------------------------ ops strip

    public string OpsLine => Comms?.Log.Count > 0 ? Comms.Log[^1] : "No comms events yet.";
    public IReadOnlyList<string> OpsLines => Comms?.Log.Reverse().ToList() ?? [];

    // ------------------------------------------------------------ apply

    public void Apply(EdgeState state)
    {
        var wasIn = _state?.InMeeting == true;
        _state = state;
        if (wasIn && !state.InMeeting) ReleaseAllLocal();
        ApplyCells();
        ApplyPeople();
        ApplyMeter();
        _applying = true;
        try
        {
            foreach (var name in Refreshed) Raise(name);
        }
        finally { _applying = false; }
    }

    private static readonly string[] Refreshed =
    [
        nameof(InMeeting), nameof(NotInMeeting), nameof(Waiting), nameof(Unavailable), nameof(DeskVisible), nameof(UnavailableText),
        nameof(HostHintVisible), nameof(GridVisible), nameof(EditVisible), nameof(EmptyDeskVisible), nameof(EmptyDeskText),
        nameof(RailText), nameof(LampLink), nameof(LampMic), nameof(LampChannel), nameof(LampTx),
        nameof(AllHot), nameof(AllArmed), nameof(AllText),
        nameof(Mics), nameof(Outputs), nameof(SelectedMic), nameof(SelectedOutput), nameof(SidetoneOn), nameof(ToneOn),
        nameof(GainText), nameof(FramesSent), nameof(ChannelSends), nameof(Underruns), nameof(SendErrors), nameof(SendErrorsWarn),
        nameof(MicDb), nameof(MicLevelName), nameof(OpsLine), nameof(OpsLines),
    ];

    private void RaiseDesk()
    {
        Raise(nameof(GridVisible));
        Raise(nameof(EditVisible));
        Raise(nameof(EmptyDeskVisible));
    }

    private void RaiseOps()
    {
        Raise(nameof(OpsLine));
        Raise(nameof(OpsLines));
    }

    // ZComms rule: a channel is a cell when someone is on it, it is named, or
    // it is keyed/latched. The grid is rebuilt only when that set (or a name)
    // changes; otherwise each cell updates in place, so a press is never
    // destroyed under the operator's finger (that eats the release).
    private void ApplyCells()
    {
        var chans = DeskVisible ? Comms!.Channels : [];
        var rows = chans.Where(c => c.Listeners > 0 || c.Label is not null || c.Keyed || c.Latched).ToList();
        var sig = string.Join("|", rows.Select(c => $"{c.Slot}:{c.Label}"));
        if (sig != _cellSig)
        {
            _cellSig = sig;
            var held = Cells.Where(c => c.Held).Select(c => c.Slot).ToHashSet();
            Cells.Clear();
            foreach (var c in rows) Cells.Add(new CommsCellViewModel(c.Slot) { Held = held.Contains(c.Slot) });
        }
        var live = 0;
        foreach (var cell in Cells)
        {
            var ch = chans.First(c => c.Slot == cell.Slot);
            cell.Name = ch.Label ?? $"CH {ch.Slot + 1}";
            var unreachable = ch.ReachOk == 0 && ch.ReachDark > 0;
            if (ch.Room is not null || unreachable)
            {
                cell.Off = true;
                cell.Hot = cell.Armed = cell.Ready = false;
                cell.StateText = ch.Room is string room ? $"in {room}" : "all in other rooms";
                cell.KeyHint = null;
                continue;
            }
            cell.Off = false;
            var hearing = ch.Listeners > 0;
            cell.Hot = ch.Keyed && hearing;
            cell.Armed = ch.Keyed && !hearing;
            cell.Ready = !ch.Keyed && hearing;
            var partial = ch.ReachDark > 0 ? $" · {ch.ReachDark} elsewhere" : "";
            cell.StateText = (ch.Keyed && hearing ? (ch.Latched ? "on air · latched" : "on air")
                : ch.Keyed ? "nobody in channel"
                : !ch.Ready ? "forming…"
                : ch.Label is null ? $"{ch.Listeners} listening"
                : hearing ? "ready" : "invite in flight") + partial;
            live++;
            cell.KeyHint = live <= 9 ? live.ToString() : null;
        }
    }

    private void ApplyPeople()
    {
        var c = DeskVisible ? Comms : null;
        var roster = c?.Roster ?? [];
        var chans = c?.Channels ?? [];
        var sig = string.Join("~", roster.Select(p => $"{p.ParticipantId}:{p.Name}:{p.Talkback}:{p.Room}:{string.Join(",", p.Channels)}"))
            + "#" + string.Join("~", chans.Select(x => x.Label ?? ""));
        if (sig == _peopleSig) return;
        _peopleSig = sig;
        People.Clear();
        foreach (var p in roster)
        {
            var chips = new List<CommsChipViewModel>();
            if (p.Talkback)
            {
                foreach (var ch in chans)
                {
                    var on = p.Channels.Contains(ch.Slot);
                    var others = roster.Where(o => o.ParticipantId != p.ParticipantId && o.Channels.Contains(ch.Slot)).Select(o => o.Name).ToList();
                    var label = ch.Label ?? $"CH {ch.Slot + 1}";
                    var tip = (on ? $"Take off {label}" : $"Put on {label}")
                        + (others.Count > 0 ? $", with {string.Join(", ", others)}" : on ? "" : " (empty)");
                    var slot = ch.Slot;
                    var pid = p.ParticipantId;
                    chips.Add(new CommsChipViewModel
                    {
                        Slot = slot, Text = (slot + 1).ToString(), On = on, Busy = !on && others.Count > 0, Tip = tip,
                        Command = new RelayCommand(() => _ = SendAsync("comms-assign", new { slot, participantId = pid, on = !on })),
                    });
                }
            }
            People.Add(new CommsPersonViewModel
            {
                Name = p.Name + (p.Room is string r ? $"  · in {r}" : ""),
                Talkback = p.Talkback,
                Chips = chips,
            });
        }
        Raise(nameof(PeopleWhy));
    }

    public string PeopleWhy => WebClientWhy;

    // 12 segments over -48..0 dBFS (ZComms mic meter scale).
    private void ApplyMeter()
    {
        var lit = MicDb is double db ? (int)Math.Round(Math.Clamp((db + 48) / 48, 0, 1) * MeterSegments) : 0;
        for (var i = 0; i < Meter.Count; i++) Meter[i].Lit = i < lit;
    }

    // ------------------------------------------------------------ keys

    public Task PressAsync(CommsCellViewModel cell)
    {
        if (cell.Off || !DeskVisible) return Task.CompletedTask;
        var ch = Comms!.Channels.FirstOrDefault(c => c.Slot == cell.Slot);
        if (_latchMode) return SendAsync("comms-latch", new { slot = cell.Slot, on = !(ch?.Latched ?? false) });
        if (cell.Held) return Task.CompletedTask;
        cell.Held = true;
        return SendAsync("comms-key", new { slot = cell.Slot, on = true });
    }

    public Task ReleaseAsync(CommsCellViewModel cell)
    {
        if (!cell.Held) return Task.CompletedTask;
        cell.Held = false;
        return SendAsync("comms-key", new { slot = cell.Slot, on = false });
    }

    public Task AllPressAsync()
    {
        if (!DeskVisible) return Task.CompletedTask;
        if (_latchMode)
        {
            var allOn = Comms!.Channels.Count > 0 && Comms.Channels.All(c => c.Latched);
            return SendAsync("comms-latch", new { all = true, on = !allOn });
        }
        if (_allHeld) return Task.CompletedTask;
        _allHeld = true;
        return SendAsync("comms-key", new { all = true, on = true });
    }

    public Task AllReleaseAsync()
    {
        if (!_allHeld) return Task.CompletedTask;
        _allHeld = false;
        return SendAsync("comms-key", new { all = true, on = false });
    }

    // Digits 1-9 key the first nine reachable cells (panel-scoped).
    public CommsCellViewModel? CellForDigit(int digit)
    {
        var live = Cells.Where(c => !c.Off).ToList();
        return digit >= 1 && digit <= live.Count ? live[digit - 1] : null;
    }

    // Focus left the screen or the window: every held key goes up.
    public async Task ReleaseAllAsync()
    {
        foreach (var c in Cells.Where(c => c.Held).ToList()) await ReleaseAsync(c);
        await AllReleaseAsync();
    }

    private void ReleaseAllLocal()
    {
        foreach (var c in Cells) c.Held = false;
        _allHeld = false;
    }

    // ------------------------------------------------------------ sends

    private Task SendGainAsync(int delta)
    {
        if (Comms is null) return Task.CompletedTask;
        var db = Math.Clamp(Comms.Mic.GainDb + delta, -20, 20);
        return db == Comms.Mic.GainDb ? Task.CompletedTask : SendAsync("comms-gain", new { db });
    }

    private Task SendAsync(string cmd, object? fields = null)
    {
        var task = SendCoreAsync(cmd, fields);
        LastSend = task;
        return task;
    }

    private async Task SendCoreAsync(string cmd, object? fields)
    {
        try
        {
            var result = await _main.SendAsync(cmd, fields);
            Error = result.Ok ? null : result.Error;
        }
        catch (Exception ex)
        {
            Error = ex.Message;
        }
    }
}
