using System.Collections.ObjectModel;
using System.Windows.Input;

namespace CoreVideoEdge.Desktop.ViewModels;

public enum CellKind { Subscribe, Iso }

public enum CellState
{
    Hidden,       // not applicable (an ISO cell in a column this guest is not routed to)
    Empty,        // routable
    Routed,       // subscribe: this guest is in this slot (node-confirmed)
    Armed,        // ISO on, not recording
    Recording,    // ISO on, writing
    Pending,      // sent, not yet confirmed by the node's state
    Failed,       // the node refused; reason on the row and in the tooltip
    Unavailable,  // cap reached, slot carries someone else, or locked
}

// One crosspoint. The node assigns slots (lowest free), so a click on any free
// subscribe cell routes the guest and the pending mark sits where the node
// will put them.
public sealed class CellViewModel : ObservableObject
{
    private readonly RoutingViewModel _owner;
    private CellState _state;
    private string _tip = "";
    private string _name = "";

    public CellViewModel(RoutingViewModel owner, ParticipantRowViewModel row, CellKind kind, int column)
    {
        _owner = owner;
        Row = row;
        Kind = kind;
        Column = column;
        ToggleCommand = new RelayCommand(() => _ = _owner.ToggleAsync(this), () => IsInteractive);
    }

    public ParticipantRowViewModel Row { get; }
    public CellKind Kind { get; }
    public int Column { get; }
    public RelayCommand ToggleCommand { get; }

    public CellState State
    {
        get => _state;
        set
        {
            if (!Set(ref _state, value)) return;
            foreach (var n in StateNames) Raise(n);
            ToggleCommand.NotifyCanExecuteChanged();
        }
    }

    public string Tip { get => _tip; set => Set(ref _tip, value); }
    public string AccessibleName { get => _name; set => Set(ref _name, value); }

    public bool IsVisible => _state != CellState.Hidden;
    public bool IsInteractive => _state is CellState.Empty or CellState.Routed or CellState.Armed or CellState.Failed;
    public bool IsRouted => _state == CellState.Routed;
    public bool IsArmed => _state == CellState.Armed;
    public bool IsRecording => _state == CellState.Recording;
    public bool IsPending => _state == CellState.Pending;
    public bool IsFailed => _state == CellState.Failed;
    public bool IsUnavailable => _state == CellState.Unavailable;
    public bool IsEmpty => _state == CellState.Empty;

    private static readonly string[] StateNames =
    [
        nameof(IsVisible), nameof(IsInteractive), nameof(IsRouted), nameof(IsArmed), nameof(IsRecording),
        nameof(IsPending), nameof(IsFailed), nameof(IsUnavailable), nameof(IsEmpty),
    ];
}

public sealed class ParticipantRowViewModel : ObservableObject
{
    private string _name = "";
    private bool _videoOn, _talking, _muted, _host, _self, _iso, _selected;
    private int? _slot;
    private string _offered = "";
    private string? _detail;
    private bool _detailIsError;

    public ParticipantRowViewModel(uint id) => Id = id;

    public uint Id { get; }
    public ObservableCollection<CellViewModel> SubscribeCells { get; } = new();
    public ObservableCollection<CellViewModel> IsoCells { get; } = new();

    public string Name { get => _name; set { if (Set(ref _name, value)) Raise(nameof(DisplayName)); } }
    public bool VideoOn { get => _videoOn; set { if (Set(ref _videoOn, value)) { Raise(nameof(StatusWords)); Raise(nameof(AccessibleName)); } } }
    public bool Talking { get => _talking; set { if (Set(ref _talking, value)) Raise(nameof(StatusWords)); } }
    public bool Muted { get => _muted; set { if (Set(ref _muted, value)) Raise(nameof(StatusWords)); } }
    public bool Host { get => _host; set { if (Set(ref _host, value)) Raise(nameof(DisplayName)); } }
    public bool Self { get => _self; set { if (Set(ref _self, value)) Raise(nameof(DisplayName)); } }
    public int? Slot { get => _slot; set { if (Set(ref _slot, value)) { Raise(nameof(SlotTag)); Raise(nameof(HasSlot)); Raise(nameof(AccessibleName)); } } }
    public bool Iso { get => _iso; set => Set(ref _iso, value); }
    public bool Selected { get => _selected; set => Set(ref _selected, value); }
    // Resolution actually received, when the node measured it.
    public string Offered { get => _offered; set { if (Set(ref _offered, value)) Raise(nameof(HasOffered)); } }
    public bool HasOffered => _offered.Length > 0;
    public string? Detail { get => _detail; set { if (Set(ref _detail, value)) Raise(nameof(HasDetail)); } }
    public bool DetailIsError { get => _detailIsError; set => Set(ref _detailIsError, value); }
    public bool HasDetail => !string.IsNullOrEmpty(_detail);

    public string DisplayName => _self ? $"{_name} (this node)" : _host ? $"{_name} (host)" : _name;
    public bool HasSlot => _slot is not null;
    public string SlotTag => _slot is int s ? $"S{s}" : "";
    public string StatusWords => string.Join(" · ", new[]
    {
        _videoOn ? "video on" : "video off",
        _muted ? "muted" : _talking ? "talking" : null,
    }.Where(w => w is not null));
    public string AccessibleName => $"{DisplayName}, {StatusWords}{(_slot is int s ? $", routed to slot {s}" : ", not routed")}";
}

public sealed class SlotHeaderViewModel : ObservableObject
{
    private Tone _tone;
    private string _raster = "";
    private string _occupant = "";
    private string _word = "";

    public SlotHeaderViewModel(int slot, CellKind kind)
    {
        Slot = slot;
        Kind = kind;
    }

    public int Slot { get; }
    public CellKind Kind { get; }
    public string Label => Kind == CellKind.Subscribe ? $"S{Slot}" : $"I{Slot}";
    public Tone Tone { get => _tone; set { if (Set(ref _tone, value)) Raise(nameof(HasDot)); } }
    public bool HasDot => _tone != Tone.Neutral;
    public string Raster { get => _raster; set => Set(ref _raster, value); }
    public string Occupant { get => _occupant; set => Set(ref _occupant, value); }
    public string Word { get => _word; set => Set(ref _word, value); }
    public string AccessibleName => $"{(Kind == CellKind.Subscribe ? "Slot" : "ISO")} {Slot}";
}

// Routing: a crosspoint grid. Rows = participants, Subscribe band S1..Sn and
// ISO band I1..In. A cell never shows "routed" before the node confirms it.
public sealed class RoutingViewModel : ObservableObject
{
    private static readonly TimeSpan FailedFor = TimeSpan.FromSeconds(12);
    private readonly MainViewModel _main;
    private readonly Dictionary<uint, ParticipantRowViewModel> _byId = new();
    private readonly Dictionary<uint, (bool On, TimeSpan At)> _pendingSub = new();
    private readonly Dictionary<int, (bool On, TimeSpan At)> _pendingIso = new();
    private readonly Dictionary<uint, (string Error, TimeSpan At)> _failedSub = new();
    private readonly Dictionary<int, (string Error, TimeSpan At)> _failedIso = new();
    private bool _videoOnOnly = true;
    private string _search = "";
    private int _cap;
    private bool _narrow;
    private bool _showIsoBand;
    private ParticipantRowViewModel? _selected;
    private TimeSpan? _rosterWaitSince;

    public RoutingViewModel(MainViewModel main)
    {
        _main = main;
        Feeds = new FeedsViewModel(main);
        ShowVideoOnCommand = new RelayCommand(() => VideoOnOnly = true);
        ShowAllCommand = new RelayCommand(() => VideoOnOnly = false);
        ShowSubscribeBandCommand = new RelayCommand(() => ShowIsoBand = false);
        ShowIsoBandCommand = new RelayCommand(() => ShowIsoBand = true);
        FreeSlotCommand = new RelayCommand(p => { if (p is int slot) _ = FreeDepartedSlotAsync(slot); });
    }

    public ICommand FreeSlotCommand { get; }

    public FeedsViewModel Feeds { get; }
    public ObservableCollection<ParticipantRowViewModel> Rows { get; } = new();
    public ObservableCollection<SlotHeaderViewModel> SubscribeHeaders { get; } = new();
    public ObservableCollection<SlotHeaderViewModel> IsoHeaders { get; } = new();
    public ICommand ShowVideoOnCommand { get; }
    public ICommand ShowAllCommand { get; }
    public ICommand ShowSubscribeBandCommand { get; }
    public ICommand ShowIsoBandCommand { get; }

    private EdgeState? S => _main.State;

    public bool VideoOnOnly
    {
        get => _videoOnOnly;
        set { if (Set(ref _videoOnOnly, value)) { Raise(nameof(ShowAll)); Rebuild(); } }
    }
    public bool ShowAll => !_videoOnOnly;
    public string Search { get => _search; set { if (Set(ref _search, value)) Rebuild(); } }

    // Under ~1100 px both bands do not fit: they become Subscribe | ISO tabs.
    public bool Narrow { get => _narrow; set { if (Set(ref _narrow, value)) RaiseBands(); } }
    public bool ShowIsoBand { get => _showIsoBand; set { if (Set(ref _showIsoBand, value)) RaiseBands(); } }
    public bool SubscribeBandVisible => !_narrow || !_showIsoBand;
    public bool IsoBandVisible => !_narrow || _showIsoBand;
    private void RaiseBands()
    {
        Raise(nameof(SubscribeBandVisible));
        Raise(nameof(IsoBandVisible));
        Raise(nameof(ShowIsoBand));
        Raise(nameof(ShowSubscribeBand));
    }
    public bool ShowSubscribeBand => !_showIsoBand;

    public ParticipantRowViewModel? SelectedRow
    {
        get => _selected;
        set
        {
            if (_selected is not null) _selected.Selected = false;
            if (Set(ref _selected, value) && value is not null) value.Selected = true;
        }
    }

    public int VideoOnCount => People.Count(p => p.VideoOn);
    public int AllCount => People.Count;
    // Counts only once the roster is known (never a "0" for "not reported").
    public bool RosterKnown => S?.Participants is not null;
    public string VideoOnLabel => RosterKnown ? $"Video on {VideoOnCount}" : "Video on";
    public string AllLabel => RosterKnown ? $"All {AllCount}" : "All";
    public string SubscribedText => $"Subscribed {_main.RoutedCount}/{_main.Cap}";
    public string IsoText => $"ISO {_main.IsoCount}/{_main.Cap}";
    public bool CapVisible => _main.Cap > 0;

    private IReadOnlyList<EdgeParticipant> People => S?.Participants?.Where(p => !p.Self).ToList() ?? [];

    // Empty / loading states.
    public bool NotJoined => S?.InMeeting != true;
    public bool RosterLoading => S?.InMeeting == true && S.Participants is null;
    public bool RosterSkeleton => RosterLoading && (_rosterWaitSince is null || _main.Clock.Now - _rosterWaitSince < TimeSpan.FromSeconds(2));
    public bool RosterWaiting => RosterLoading && !RosterSkeleton;
    public string RosterWaitText => _rosterWaitSince is TimeSpan t
        ? $"Waiting for the roster from Zoom · {Format.Elapsed(_main.Clock.Now - t)}" : "";
    public bool NoVideoOn => S?.InMeeting == true && S.Participants is not null && _videoOnOnly && VideoOnCount == 0 && AllCount > 0;
    public bool NobodyHere => S?.InMeeting == true && S.Participants is not null && AllCount == 0;
    public bool NoSearchMatch => S?.InMeeting == true && Rows.Count == 0 && !NoVideoOn && !NobodyHere && _search.Length > 0;
    public bool MatrixVisible => S?.InMeeting == true && Rows.Count > 0;
    public bool Recording => S?.Recording == true;

    public IReadOnlyList<EdgeSubscription> Departed => S?.Subscriptions.Where(x => !x.Present).ToList() ?? [];
    public bool HasDeparted => Departed.Count > 0;
    public string DepartedText => HasDeparted
        ? "Left the meeting, slot held for their return: " + string.Join(", ", Departed.Select(d => $"{d.Name} (S{d.Slot})"))
        : "";

    public void Apply(EdgeState s)
    {
        Feeds.Apply(s);
        var now = _main.Clock.Now;
        if (s.InMeeting && s.Participants is null) _rosterWaitSince ??= now;
        else _rosterWaitSince = null;

        // A pending mark clears when the node's state matches what was asked.
        foreach (var (id, p) in _pendingSub.ToList())
        {
            var routed = s.Subscriptions.Any(x => x.ParticipantId == id);
            if (routed == p.On) _pendingSub.Remove(id);
        }
        foreach (var (slot, p) in _pendingIso.ToList())
        {
            var sub = s.Subscriptions.FirstOrDefault(x => x.Slot == slot);
            if (sub is null || sub.Iso == p.On) _pendingIso.Remove(slot);
        }
        if (s.MaxSubscriptions != _cap)
        {
            _cap = s.MaxSubscriptions;
            SubscribeHeaders.Clear();
            IsoHeaders.Clear();
            for (var i = 1; i <= _cap; ++i)
            {
                SubscribeHeaders.Add(new SlotHeaderViewModel(i, CellKind.Subscribe));
                IsoHeaders.Add(new SlotHeaderViewModel(i, CellKind.Iso));
            }
            foreach (var row in _byId.Values) EnsureCells(row);
        }
        Rebuild();
    }

    public void Tick()
    {
        var now = _main.Clock.Now;
        var changed = false;
        foreach (var (id, f) in _failedSub.ToList()) if (now - f.At > FailedFor) { _failedSub.Remove(id); changed = true; }
        foreach (var (slot, f) in _failedIso.ToList()) if (now - f.At > FailedFor) { _failedIso.Remove(slot); changed = true; }
        if (changed || RosterLoading) Rebuild();
    }

    private void EnsureCells(ParticipantRowViewModel row)
    {
        while (row.SubscribeCells.Count > _cap) row.SubscribeCells.RemoveAt(row.SubscribeCells.Count - 1);
        while (row.IsoCells.Count > _cap) row.IsoCells.RemoveAt(row.IsoCells.Count - 1);
        for (var i = row.SubscribeCells.Count + 1; i <= _cap; ++i) row.SubscribeCells.Add(new CellViewModel(this, row, CellKind.Subscribe, i));
        for (var i = row.IsoCells.Count + 1; i <= _cap; ++i) row.IsoCells.Add(new CellViewModel(this, row, CellKind.Iso, i));
    }

    // Rows are updated in place (keyed by participant id), never rebuilt, so
    // focus and scroll survive several state lines a second.
    private void Rebuild()
    {
        var s = S;
        var people = People;
        var visible = people
            .Where(p => !_videoOnOnly || p.VideoOn || p.Slot is not null)
            .Where(p => _search.Length == 0 || p.Name.Contains(_search, StringComparison.OrdinalIgnoreCase))
            .ToList();

        for (var i = Rows.Count - 1; i >= 0; --i)
            if (visible.All(p => p.Id != Rows[i].Id)) Rows.RemoveAt(i);
        for (var index = 0; index < visible.Count; ++index)
        {
            var p = visible[index];
            if (!_byId.TryGetValue(p.Id, out var row))
            {
                row = new ParticipantRowViewModel(p.Id);
                _byId[p.Id] = row;
            }
            EnsureCells(row);
            ApplyRow(row, p, s!);
            var at = Rows.IndexOf(row);
            if (at < 0) Rows.Insert(index, row);
            else if (at != index) Rows.Move(at, index);
        }
        foreach (var id in _byId.Keys.Where(id => people.All(p => p.Id != id)).ToList()) _byId.Remove(id);
        if (_selected is not null && !Rows.Contains(_selected)) SelectedRow = null;
        if (s is not null) ApplyHeaders(s);
        foreach (var n in Names) Raise(n);
    }

    private int? NextFreeSlot(EdgeState s)
    {
        for (var i = 1; i <= s.MaxSubscriptions; ++i)
            if (s.Subscriptions.All(x => x.Slot != i)) return i;
        return null;
    }

    private void ApplyRow(ParticipantRowViewModel row, EdgeParticipant p, EdgeState s)
    {
        row.Name = p.Name;
        row.VideoOn = p.VideoOn;
        row.Talking = p.Talking;
        row.Muted = p.Muted;
        row.Host = p.Host;
        row.Self = p.Self;
        row.Slot = p.Slot;
        row.Iso = p.Slot is not null && p.Iso;
        row.Offered = p.ReceivedWidth is int w && p.ReceivedHeight is int h
            ? Format.Size(w, h) + (p.Receiving ? "" : " stalled")
            : "";

        var pendingSub = _pendingSub.TryGetValue(p.Id, out var ps) ? ps : ((bool On, TimeSpan At)?)null;
        var failedSub = _failedSub.TryGetValue(p.Id, out var fs) ? fs.Error : null;
        var nextFree = NextFreeSlot(s);
        var atCap = nextFree is null;
        var recordingThisIso = s.Recording && row.Iso;

        foreach (var cell in row.SubscribeCells)
        {
            var k = cell.Column;
            var occupant = s.Subscriptions.FirstOrDefault(x => x.Slot == k);
            CellState state;
            string tip;
            if (pendingSub is { } pend && ((pend.On && k == nextFree) || (!pend.On && p.Slot == k)))
            {
                state = CellState.Pending;
                tip = pend.On ? $"Routing {p.Name} to S{k}. Waiting for the node to confirm." : $"Unrouting {p.Name} from S{k}.";
            }
            else if (p.Slot == k)
            {
                state = recordingThisIso ? CellState.Unavailable : CellState.Routed;
                tip = recordingThisIso
                    ? $"{p.Name} is on S{k} and its ISO is recording. Stop recording to unroute."
                    : $"{p.Name} is on S{k}. Select to unroute.";
                if (recordingThisIso) state = CellState.Routed;  // shown routed; the click is refused below
            }
            else if (p.Slot is not null)
            {
                state = CellState.Hidden;
                tip = "";
            }
            else if (failedSub is not null && k == (nextFree ?? k))
            {
                state = CellState.Failed;
                tip = failedSub;
            }
            else if (atCap)
            {
                state = CellState.Unavailable;
                tip = $"All {s.MaxSubscriptions} slots in use. Unroute one first.";
            }
            else if (occupant is not null)
            {
                state = CellState.Unavailable;
                tip = $"S{k} carries {occupant.Name}.";
            }
            else
            {
                state = CellState.Empty;
                tip = $"Route {p.Name}. The node assigns the next free slot (S{nextFree}).";
            }
            if (p.Slot is not null && p.Slot != k) state = CellState.Hidden;
            cell.State = state;
            cell.Tip = tip;
            cell.AccessibleName = $"Subscribe, {p.Name}, slot {k}: {Words(state)}";
        }

        var failedIso = p.Slot is int sl && _failedIso.TryGetValue(sl, out var fi) ? fi.Error : null;
        foreach (var cell in row.IsoCells)
        {
            var k = cell.Column;
            CellState state;
            string tip;
            if (p.Slot != k)
            {
                state = CellState.Hidden;
                tip = "";
            }
            else if (_pendingIso.TryGetValue(k, out var pi))
            {
                state = CellState.Pending;
                tip = pi.On ? $"Arming the ISO for {p.Name}." : $"Disarming the ISO for {p.Name}.";
            }
            else if (failedIso is not null)
            {
                state = CellState.Failed;
                tip = failedIso;
            }
            else if (s.Recording)
            {
                state = row.Iso ? CellState.Recording : CellState.Unavailable;
                tip = row.Iso ? $"Recording the ISO for {p.Name}. ISO selection is fixed while recording."
                              : "ISO selection is fixed while recording.";
            }
            else
            {
                state = row.Iso ? CellState.Armed : CellState.Empty;
                tip = row.Iso ? $"ISO armed for {p.Name}. Select to disarm." : $"Arm an ISO for {p.Name}.";
            }
            cell.State = state;
            cell.Tip = tip;
            cell.AccessibleName = $"ISO, {p.Name}, slot {k}: {Words(state)}";
        }

        row.Detail = failedSub ?? failedIso ?? (pendingSub is not null ? "Waiting for the node…" : null);
        row.DetailIsError = failedSub is not null || failedIso is not null;
    }

    private void ApplyHeaders(EdgeState s)
    {
        foreach (var header in SubscribeHeaders.Concat(IsoHeaders))
        {
            var sub = s.Subscriptions.FirstOrDefault(x => x.Slot == header.Slot);
            var p = sub is null ? null : s.Participants?.FirstOrDefault(x => x.Id == sub.ParticipantId);
            if (sub is null || (header.Kind == CellKind.Iso && !sub.Iso))
            {
                header.Tone = Tone.Neutral;
                header.Raster = "";
                header.Occupant = "";
                header.Word = "";
                continue;
            }
            header.Occupant = sub.Name;
            if (header.Kind == CellKind.Iso)
            {
                header.Tone = s.Recording ? Tone.Rec : Tone.Accent;
                header.Word = s.Recording ? "rec" : "armed";
                header.Raster = "";
                continue;
            }
            if (!sub.Present)
            {
                header.Tone = Tone.Warn;
                header.Word = "left";
                header.Raster = "";
            }
            else if (p?.ReceivedHeight is int h)
            {
                header.Tone = p.Receiving ? Tone.Live : Tone.Error;
                header.Word = p.Receiving ? "live" : "stalled";
                header.Raster = Format.Raster(h, p.ReceivedFps);
            }
            else
            {
                header.Tone = Tone.Warn;
                header.Word = "waiting";
                header.Raster = "";
            }
        }
    }

    public async Task ToggleAsync(CellViewModel cell)
    {
        var s = S;
        if (s is null) return;
        var row = cell.Row;
        if (cell.Kind == CellKind.Subscribe)
        {
            var on = row.Slot is null;
            if (!on && s.Recording && row.Iso)
            {
                _failedSub[row.Id] = ($"{row.Name}'s ISO is recording. Stop recording before unrouting.", _main.Clock.Now);
                Rebuild();
                return;
            }
            _failedSub.Remove(row.Id);
            _pendingSub[row.Id] = (on, _main.Clock.Now);
            Rebuild();
            var result = await _main.SendAsync("subscribe", new { participantId = row.Id, on });
            if (!result.Ok)
            {
                _pendingSub.Remove(row.Id);
                _failedSub[row.Id] = (result.Error ?? "The node refused.", _main.Clock.Now);
            }
            Rebuild();
        }
        else if (row.Slot is int slot)
        {
            var on = !row.Iso;
            _failedIso.Remove(slot);
            _pendingIso[slot] = (on, _main.Clock.Now);
            Rebuild();
            var result = await _main.SendAsync("iso", new { slot, on });
            if (!result.Ok)
            {
                _pendingIso.Remove(slot);
                _failedIso[slot] = (result.Error ?? "The node refused.", _main.Clock.Now);
            }
            Rebuild();
        }
    }

    public async Task FreeDepartedSlotAsync(int slot)
    {
        var result = await _main.SendAsync("unsubscribe-slot", new { slot });
        if (!result.Ok) _main.ReportError(result.Error ?? "The node refused.");
    }

    private static string Words(CellState s) => s switch
    {
        CellState.Empty => "not routed",
        CellState.Routed => "routed",
        CellState.Armed => "armed",
        CellState.Recording => "recording",
        CellState.Pending => "pending",
        CellState.Failed => "failed",
        CellState.Unavailable => "unavailable",
        _ => "",
    };

    private static readonly string[] Names =
    [
        nameof(RosterKnown), nameof(VideoOnCount), nameof(AllCount), nameof(VideoOnLabel), nameof(AllLabel), nameof(SubscribedText), nameof(IsoText),
        nameof(CapVisible), nameof(NotJoined), nameof(RosterLoading), nameof(RosterSkeleton), nameof(RosterWaiting),
        nameof(RosterWaitText), nameof(NoVideoOn), nameof(NobodyHere), nameof(NoSearchMatch), nameof(MatrixVisible), nameof(Recording),
        nameof(Departed), nameof(HasDeparted), nameof(DepartedText),
    ];
}
