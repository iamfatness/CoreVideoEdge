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

    // The Zoom feed this guest's slot asks for (routed rows only). Set by the
    // operator through the picker (sends set-resolution) or by the node's state
    // (SetResolutionFromNode, which never sends).
    private int _resolution = 1080;
    private bool _offeredBelow;
    private string _offeredTip = "";
    internal Action<ParticipantRowViewModel, int>? ResolutionPicked { get; set; }
    public static IReadOnlyList<int> ResolutionOptions { get; } = [1080, 720, 360];
    public int Resolution
    {
        get => _resolution;
        set { if (Set(ref _resolution, value)) ResolutionPicked?.Invoke(this, value); }
    }
    internal void SetResolutionFromNode(int value) => Set(ref _resolution, value, nameof(Resolution));

    // The slot's output rate (ISO + SRT feed); 0 = as Zoom sends.
    private int _fps;
    internal Action<ParticipantRowViewModel, int>? FpsPicked { get; set; }
    public static IReadOnlyList<int> FpsOptions { get; } = [0, 30, 60];
    public static Avalonia.Data.Converters.FuncValueConverter<int, string> FpsLabel { get; } =
        new(f => f == 0 ? "Source" : $"{f} fps");
    public int Fps
    {
        get => _fps;
        set { if (Set(ref _fps, value)) FpsPicked?.Invoke(this, value); }
    }
    internal void SetFpsFromNode(int value) => Set(ref _fps, value, nameof(Fps));
    // The feed pickers lock while this slot's ISO records (the node refuses).
    private bool _feedLocked;
    public bool FeedLocked { get => _feedLocked; set => Set(ref _feedLocked, value); }
    public bool OfferedBelowAsked { get => _offeredBelow; set => Set(ref _offeredBelow, value); }
    public string OfferedTip { get => _offeredTip; set => Set(ref _offeredTip, value); }

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
    // Delivered below what the slot asks Zoom for (amber), and why.
    private bool _rasterBelow;
    private string _tip = "";
    public bool RasterBelowAsked { get => _rasterBelow; set => Set(ref _rasterBelow, value); }
    public string Tip { get => _tip; set => Set(ref _tip, value); }
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
    private readonly Dictionary<uint, (bool On, int Slot, TimeSpan At)> _pendingSub = new();
    private readonly Dictionary<int, (bool On, TimeSpan At)> _pendingIso = new();
    private readonly Dictionary<uint, (string Error, int Slot, TimeSpan At)> _failedSub = new();
    private int _defaultResolution = 1080;
    private int _defaultFps;  // 0 = as Zoom sends
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
        Feed1080Command = new RelayCommand(() => DefaultResolution = 1080);
        Feed720Command = new RelayCommand(() => DefaultResolution = 720);
        Feed360Command = new RelayCommand(() => DefaultResolution = 360);
        FpsSourceCommand = new RelayCommand(() => DefaultFps = 0);
        Fps30Command = new RelayCommand(() => DefaultFps = 30);
        Fps60Command = new RelayCommand(() => DefaultFps = 60);
    }

    public ICommand FreeSlotCommand { get; }

    // The Zoom feed new routes ask for (owner 2026-11-09: chosen on Routing).
    // 1080p unless the operator explicitly picks less; each routed slot then
    // carries its own value, changed from its row.
    public int DefaultResolution
    {
        get => _defaultResolution;
        set
        {
            if (!ParticipantRowViewModel.ResolutionOptions.Contains(value) || !Set(ref _defaultResolution, value)) return;
            Raise(nameof(Feed1080)); Raise(nameof(Feed720)); Raise(nameof(Feed360));
        }
    }
    public bool Feed1080 => _defaultResolution == 1080;
    public bool Feed720 => _defaultResolution == 720;
    public bool Feed360 => _defaultResolution == 360;
    public ICommand Feed1080Command { get; }
    public ICommand Feed720Command { get; }
    public ICommand Feed360Command { get; }
    // The frame rate new routes write and send on (ISO + SRT): Zoom cannot be
    // asked for a rate, so this paces Edge's outputs. 0 = as Zoom sends.
    public int DefaultFps
    {
        get => _defaultFps;
        set
        {
            if (!ParticipantRowViewModel.FpsOptions.Contains(value) || !Set(ref _defaultFps, value)) return;
            Raise(nameof(FpsSource)); Raise(nameof(Fps30)); Raise(nameof(Fps60));
        }
    }
    public bool FpsSource => _defaultFps == 0;
    public bool Fps30 => _defaultFps == 30;
    public bool Fps60 => _defaultFps == 60;
    public ICommand FpsSourceCommand { get; }
    public ICommand Fps30Command { get; }
    public ICommand Fps60Command { get; }

    // The most recent set-resolution / set-fps send (tests await it).
    public Task? LastResolutionSend { get; private set; }

    private void OnResolutionPicked(ParticipantRowViewModel row, int height)
    {
        if (row.Slot is not int slot) return;
        LastResolutionSend = SendResolutionAsync(row, slot, height);
    }

    private void OnFpsPicked(ParticipantRowViewModel row, int fps)
    {
        if (row.Slot is not int slot) return;
        LastResolutionSend = SendFpsAsync(row, slot, fps);
    }

    private async Task SendFpsAsync(ParticipantRowViewModel row, int slot, int fps)
    {
        var result = await _main.SendAsync("set-fps", new { slot, fps });
        if (!result.Ok)
        {
            _failedSub[row.Id] = (result.Error ?? "The node refused.", slot, _main.Clock.Now);
            Rebuild();
        }
    }

    private async Task SendResolutionAsync(ParticipantRowViewModel row, int slot, int height)
    {
        var result = await _main.SendAsync("set-resolution", new { slot, resolution = height });
        if (!result.Ok)
        {
            _failedSub[row.Id] = (result.Error ?? "The node refused.", slot, _main.Clock.Now);
            Rebuild();   // the picker returns to the node's value with the next state line
        }
    }

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
            var at = s.Subscriptions.FirstOrDefault(x => x.ParticipantId == id && x.Present)?.Slot;
            if (p.On ? at == p.Slot : at is null) _pendingSub.Remove(id);
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
                row = new ParticipantRowViewModel(p.Id) { ResolutionPicked = OnResolutionPicked, FpsPicked = OnFpsPicked };
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
        var sub = p.Slot is int mine ? s.Subscriptions.FirstOrDefault(x => x.Slot == mine) : null;
        if (sub is not null)
        {
            row.SetResolutionFromNode(sub.Resolution);
            row.SetFpsFromNode(sub.Fps);
        }
        string? zoomNote = null;
        if (p.ReceivedWidth is int w && p.ReceivedHeight is int h)
        {
            row.Offered = Format.Size(w, h) + (p.Receiving ? "" : " stalled");
            var asked = sub?.Resolution ?? 1080;
            row.OfferedBelowAsked = h < asked;
            row.OfferedTip = sub?.ZoomGranted is int granted
                ? $"Asked for {asked}p. Zoom accepted {granted}p and is sending {Format.Size(w, h)}." +
                  (h < granted ? " What Zoom sends on an accepted subscription is decided by the guest's camera, client and HD settings, and by Zoom." : "")
                : $"Asked for {asked}p. Receiving {Format.Size(w, h)}.";
        }
        else
        {
            row.Offered = "";
            row.OfferedBelowAsked = false;
            row.OfferedTip = "";
        }
        if (sub is { ZoomFailed: true })
            zoomNote = "Zoom refused every video resolution for this guest.";
        else if (sub is { ZoomRefused: int refused } && sub.ZoomGranted is int got && got < sub.Resolution)
            zoomNote = $"The Zoom SDK refused {refused}p for this guest (code {sub.ZoomRefusedCode}); subscribed at {got}p.";

        var pendingSub = _pendingSub.TryGetValue(p.Id, out var ps) ? ps : ((bool On, int Slot, TimeSpan At)?)null;
        var failed = _failedSub.TryGetValue(p.Id, out var fs) ? fs : ((string Error, int Slot, TimeSpan At)?)null;
        var failedSub = failed?.Error;
        var atCap = NextFreeSlot(s) is null;
        var recordingThisIso = s.Recording && row.Iso;
        row.FeedLocked = recordingThisIso;

        // Every crosspoint is drawn (owner 2026-11-09). The operator picks the
        // slot: a free cell routes this guest to THAT slot.
        foreach (var cell in row.SubscribeCells)
        {
            var k = cell.Column;
            var occupant = s.Subscriptions.FirstOrDefault(x => x.Slot == k);
            CellState state;
            string tip;
            if (pendingSub is { } pend && k == pend.Slot)
            {
                state = CellState.Pending;
                tip = pend.On ? $"Routing {p.Name} to S{k}. Waiting for the node to confirm." : $"Unrouting {p.Name} from S{k}.";
            }
            else if (p.Slot == k)
            {
                state = CellState.Routed;  // while its ISO records the click is refused with the reason
                tip = recordingThisIso
                    ? $"{p.Name} is on S{k} and its ISO is recording. Stop recording to unroute."
                    : $"{p.Name} is on S{k}. Select to unroute.";
            }
            else if (failed is { } f && f.Slot == k)
            {
                state = CellState.Failed;
                tip = f.Error;
            }
            // Every other crosspoint is live (owner 2026-11-09: no greying,
            // no reflow). The node moves or replaces, or refuses with a reason.
            else if (occupant is not null)
            {
                state = CellState.Empty;
                tip = p.Slot is int from
                    ? $"Move {p.Name} from S{from} to S{k}, replacing {occupant.Name}."
                    : $"Replace {occupant.Name} on S{k} with {p.Name}.";
            }
            else if (p.Slot is int at)
            {
                state = CellState.Empty;
                tip = $"Move {p.Name} from S{at} to S{k}.";
            }
            else
            {
                state = CellState.Empty;
                tip = atCap ? $"All {s.MaxSubscriptions} slots in use." : $"Route {p.Name} to S{k} at {_defaultResolution}p.";
            }
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
                // Live too: an ISO cell on another column routes (or moves)
                // the guest to that slot with its ISO armed.
                state = s.Recording ? CellState.Unavailable : CellState.Empty;
                tip = s.Recording ? "ISO selection is fixed while recording."
                    : p.Slot is int at ? $"Move {p.Name} from S{at} to S{k} (ISO I{k})." : $"Route {p.Name} to S{k} with an ISO.";
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

        // Pending shows in the cell only: a pending text line pushed every row
        // below down and back up on each click (owner: "refreshes weird").
        row.Detail = failedSub ?? failedIso ?? zoomNote;
        row.DetailIsError = failedSub is not null || failedIso is not null || zoomNote is not null;
    }

    private void ApplyHeaders(EdgeState s)
    {
        foreach (var header in SubscribeHeaders.Concat(IsoHeaders))
        {
            var sub = s.Subscriptions.FirstOrDefault(x => x.Slot == header.Slot);
            var p = sub is null ? null : s.Participants?.FirstOrDefault(x => x.Id == sub.ParticipantId);
            header.RasterBelowAsked = false;
            if (sub is null || (header.Kind == CellKind.Iso && !sub.Iso))
            {
                header.Tone = Tone.Neutral;
                header.Raster = "";
                header.Occupant = "";
                header.Word = "";
                header.Tip = header.Kind == CellKind.Subscribe ? $"S{header.Slot}: free" : $"I{header.Slot}: no ISO";
                continue;
            }
            header.Occupant = sub.Name;
            header.Tip = sub.Name;
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
                header.RasterBelowAsked = h < sub.Resolution;
                var row = _byId.TryGetValue(p.Id, out var r) ? r : null;
                header.Tip = $"{sub.Name}. " + (row?.OfferedTip is { Length: > 0 } t ? t : $"Asked for {sub.Resolution}p.");
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
        if (cell.Kind == CellKind.Subscribe || row.Slot != cell.Column)
        {
            // A cell on the guest's own slot unroutes; any other column takes
            // the guest to that slot (route, move or replace).
            var on = row.Slot != cell.Column;
            if (!on && s.Recording && row.Iso)
            {
                _failedSub[row.Id] = ($"{row.Name}'s ISO is recording. Stop recording before unrouting.", cell.Column, _main.Clock.Now);
                Rebuild();
                return;
            }
            _failedSub.Remove(row.Id);
            _pendingSub[row.Id] = (on, cell.Column, _main.Clock.Now);
            Rebuild();
            var result = on
                ? row.Slot is null
                    ? await _main.SendAsync("subscribe", new { participantId = row.Id, on, slot = cell.Column, resolution = _defaultResolution, fps = _defaultFps })
                    : await _main.SendAsync("subscribe", new { participantId = row.Id, on, slot = cell.Column })
                : await _main.SendAsync("subscribe", new { participantId = row.Id, on });
            if (!result.Ok)
            {
                _pendingSub.Remove(row.Id);
                _failedSub[row.Id] = (result.Error ?? "The node refused.", cell.Column, _main.Clock.Now);
            }
            Rebuild();
        }
        else if (row.Slot is int slot)
        {
            var on = !row.Iso;  // own ISO column: arm / disarm
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
