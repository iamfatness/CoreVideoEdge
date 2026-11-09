using System.Windows.Input;
using CoreVideoEdge.Desktop.Services;

namespace CoreVideoEdge.Desktop.ViewModels;

public enum EngineVisual { Unknown, Off, Standby, Requesting, On, Moving, Failed }

// Connect: three sequential steps (sign in, join, engine). Brief section 4.1.
public sealed class ConnectViewModel : ObservableObject
{
    private readonly MainViewModel _main;
    private readonly IAccountService? _account;
    private readonly AppOptions _options;

    private bool _signedIn;
    private bool _signInPending;
    private TimeSpan? _signInSince;
    private string? _accountError;
    private string _meetingLink = "";
    private string? _joinError;
    private TimeSpan? _joiningSince;
    private bool _joinSending;
    private bool _confirmLeave;
    private bool? _enginePendingTarget;
    private string? _engineError;
    private TimeSpan? _requestingSince;

    public ConnectViewModel(MainViewModel main, IAccountService? account, AppOptions options)
    {
        _main = main;
        _account = account;
        _options = options;
        SignInCommand = new AsyncCommand(SignInAsync, main.ReportError, () => _account is not null && !_signedIn);
        SignOutCommand = new AsyncCommand(SignOutAsync, main.ReportError, () => _account is not null && _signedIn && _main.State?.CanJoin != false);
        JoinCommand = new AsyncCommand(() => JoinAsync(false), main.ReportError, () => CanJoin);
        JoinEndOtherCommand = new AsyncCommand(() => JoinAsync(true), main.ReportError, () => CanJoin);
        LeaveCommand = new AsyncCommand(LeaveRequestedAsync, main.ReportError, () => LeaveVisible);
        ConfirmLeaveCommand = new AsyncCommand(LeaveAsync, main.ReportError);
        CancelLeaveCommand = new RelayCommand(() => ConfirmLeaveVisible = false);
        ToggleEngineCommand = new AsyncCommand(ToggleEngineAsync, main.ReportError, () => EngineEnabled);
        RetryEngineCommand = new AsyncCommand(RetryEngineAsync, main.ReportError, () => EngineEnabled);
    }

    public AsyncCommand SignInCommand { get; }
    public AsyncCommand SignOutCommand { get; }
    public AsyncCommand JoinCommand { get; }
    public AsyncCommand JoinEndOtherCommand { get; }
    public AsyncCommand LeaveCommand { get; }
    public AsyncCommand ConfirmLeaveCommand { get; }
    public ICommand CancelLeaveCommand { get; }
    public AsyncCommand ToggleEngineCommand { get; }
    public AsyncCommand RetryEngineCommand { get; }

    public bool HasJoinedThisSession { get; private set; }
    public bool LastJoinEndedOther { get; private set; }

    private EdgeState? S => _main.State;
    private TimeSpan Now => _main.Clock.Now;

    // ------------------------------------------------------------ 1. sign in

    public bool SignedIn => _signedIn;
    public bool SignInPending => _signInPending && !_signedIn;
    public bool SignInVisible => !_signedIn;
    public string SignInTitle => "Sign in to Zoom";
    public Tone SignInTone => _signedIn ? Tone.Live : _accountError is not null ? Tone.Error : SignInPending ? Tone.Warn : Tone.Neutral;
    public string SignInDetail
    {
        get
        {
            if (_accountError is not null) return _accountError;
            if (_signedIn) return "Signed in with Zoom. This node joins as that account.";
            if (_account is null) return "Sign-in is not available in this mode.";
            if (SignInPending)
                return _signInSince is TimeSpan since
                    ? $"Waiting for Zoom sign-in in your browser · {Format.Elapsed(Now - since)}"
                    : "Finish signing in in your browser.";
            return "Use this node's own Zoom account, not the one CoreVideo Pro uses. Two clients on one account collide.";
        }
    }
    public string SignInButtonText => SignInPending ? "Open sign-in again" : "Sign in with Zoom";

    public async Task RefreshAccountAsync()
    {
        if (_account is null) return;
        try
        {
            var status = await _account.GetStatusAsync();
            _signedIn = status.SignedIn;
            _signInPending = status.Pending;
            if (_signedIn) _signInSince = null;
            _accountError = null;
        }
        catch (Exception ex)
        {
            _accountError = ex.Message;
        }
        RaiseAll();
    }

    // For the headless screenshot mode, which has no token store.
    public void SetAccountForPreview(bool signedIn, bool pending = false)
    {
        _signedIn = signedIn;
        _signInPending = pending;
        _signInSince = pending ? Now - TimeSpan.FromSeconds(14) : null;
        RaiseAll();
    }

    private async Task SignInAsync()
    {
        _accountError = null;
        try
        {
            await _account!.BeginSignInAsync();
            _signInPending = true;
            _signInSince = Now;
        }
        catch (Exception ex)
        {
            _accountError = ex.Message;
        }
        RaiseAll();
    }

    public async Task HandleCallbackAsync(string url)
    {
        if (_account is null) return;
        try
        {
            await _account.HandleCallbackAsync(url);
            _accountError = null;
        }
        catch (Exception ex)
        {
            _accountError = ex.Message;
            RaiseAll();
            return;
        }
        await RefreshAccountAsync();
    }

    private async Task SignOutAsync()
    {
        await _account!.SignOutAsync();
        await RefreshAccountAsync();
    }

    // ------------------------------------------------------------ 2. join

    public bool JoinStepEnabled => _signedIn || _options.JoinWithoutSignIn;
    public string MeetingLink { get => _meetingLink; set { if (Set(ref _meetingLink, value)) RaiseAll(); } }
    public bool Joining => _joinSending || S?.Phase is "starting-engine" or "authenticating" or "joining";
    public bool Joined => S?.InMeeting == true;
    public bool CanJoin => JoinStepEnabled && S?.CanJoin == true && !Joining && !string.IsNullOrWhiteSpace(_meetingLink);
    public bool MeetingFieldEnabled => JoinStepEnabled && S?.CanJoin == true && !Joining;
    public bool JoinVisible => !Joined && S?.Phase != "leaving";
    public bool LeaveVisible => S is not null && !S.CanJoin && !Joining;
    public bool CanEndOther => S?.CanEndOtherSession == true && S.CanJoin;
    public bool ConfirmLeaveVisible { get => _confirmLeave; private set { if (Set(ref _confirmLeave, value)) RaiseAll(); } }
    public string JoinButtonText => Joining
        ? _joiningSince is TimeSpan since ? $"Joining… {Format.Elapsed(Now - since)}" : "Joining…"
        : "Join";
    public Tone JoinTone => Joined ? (S!.MovingRooms ? Tone.Warn : Tone.Live) : JoinError is not null ? Tone.Error : Joining ? Tone.Warn : Tone.Neutral;
    public string JoinDetail
    {
        get
        {
            if (!JoinStepEnabled) return "Sign in first.";
            var s = S;
            if (s is null) return "Starting the Edge node…";
            if (s.MovingRooms) return $"Meeting {Format.MeetingId(s.MeetingId ?? "")} · Zoom is moving this node between rooms. Routing and the take are kept.";
            if (s.InMeeting)
            {
                var people = s.Participants?.Count(p => !p.Self);
                return people is int n
                    ? $"Meeting {Format.MeetingId(s.MeetingId ?? "")} · {Format.Plural(n, "participant", "participants")} besides this node"
                    : $"Meeting {Format.MeetingId(s.MeetingId ?? "")} · waiting for the roster";
            }
            if (s.Phase == "leaving") return "Leaving the meeting…";
            if (Joining) return MainViewModel.PhaseWords(s.Phase) switch { var w => char.ToUpperInvariant(w[0]) + w[1..] + "…" };
            return "Not in a meeting.";
        }
    }
    public string? JoinError => _joinError ?? (S?.Phase == "failed" ? S.Error : null);
    public bool JoinErrorVisible => JoinError is not null;

    public async Task JoinAsync(bool endOtherMeeting)
    {
        _joinError = null;
        _joinSending = true;
        _joiningSince = Now;
        LastJoinEndedOther = endOtherMeeting;
        RaiseAll();
        try
        {
            string? sdkJwt = null, userZak = null;
            if (_signedIn && _account is not null)
            {
                try
                {
                    (sdkJwt, userZak) = await _account.GetJoinCredentialsAsync();
                }
                catch (Exception ex)
                {
                    _joinError = $"{ex.Message} Sign in again, then join.";
                    await RefreshAccountAsync();
                    return;
                }
            }
            var result = await _main.SendAsync("join", new { meeting = _meetingLink.Trim(), sdkJwt, userZak, endOtherMeeting });
            if (!result.Ok) _joinError = result.Error;
            else HasJoinedThisSession = true;
        }
        finally
        {
            _joinSending = false;
            RaiseAll();
        }
    }

    private Task LeaveRequestedAsync()
    {
        // Leaving stops the take; confirm only when ISOs are recording.
        if (S?.Recording == true)
        {
            ConfirmLeaveVisible = true;
            return Task.CompletedTask;
        }
        return LeaveAsync();
    }

    private async Task LeaveAsync()
    {
        ConfirmLeaveVisible = false;
        var result = await _main.SendAsync("leave");
        _joinError = result.Ok ? null : result.Error;
        RaiseAll();
    }

    // ------------------------------------------------------------ 3. engine

    public bool EngineEnabled => JoinStepEnabled && S?.EngineOn is not null && _enginePendingTarget is null &&
                                 !(S.EngineOn == true && S.Recording);
    public bool EnginePending => _enginePendingTarget is not null;

    public EngineVisual EngineVisual
    {
        get
        {
            var s = S;
            if (s?.EngineOn is not bool on) return EngineVisual.Unknown;
            if (!on) return EngineVisual.Off;
            if (!s.InMeeting) return EngineVisual.Standby;
            if (s.MovingRooms) return EngineVisual.Moving;
            if (s.RawMediaActive == true) return EngineVisual.On;
            if (RawMediaRefusal(s) is not null) return EngineVisual.Failed;
            return EngineVisual.Requesting;
        }
    }

    // The engine reports a refused raw-media start as a warning line.
    private static string? RawMediaRefusal(EdgeState s) =>
        s.EngineWarnings.FirstOrDefault(w => w.Contains("raw_media_start_failed", StringComparison.Ordinal));

    public bool EngineIsOn => EngineVisual is EngineVisual.On or EngineVisual.Requesting or EngineVisual.Moving or EngineVisual.Standby or EngineVisual.Failed;
    public Tone EngineTone => EngineVisual switch
    {
        EngineVisual.On => Tone.Live,
        EngineVisual.Requesting or EngineVisual.Moving => Tone.Warn,
        EngineVisual.Failed => Tone.Error,
        _ => Tone.Neutral,
    };
    public bool EnginePulse => EngineVisual is EngineVisual.Requesting or EngineVisual.Moving || EnginePending;

    public string EngineTitle => EngineVisual switch
    {
        EngineVisual.Off => "Engine off",
        EngineVisual.Standby => "Engine armed",
        EngineVisual.Requesting => "Engine on, requesting",
        EngineVisual.On => "Engine on",
        EngineVisual.Moving => "Engine on, moving rooms",
        EngineVisual.Failed => "Engine on, raw media refused",
        _ => "Engine",
    };

    public string EngineDetail
    {
        get
        {
            var s = S;
            if (!JoinStepEnabled) return "Sign in first.";
            if (_engineError is not null) return _engineError;
            if (s?.EngineOn is null) return "The node has not reported the engine state.";
            if (_enginePendingTarget is bool target) return target ? "Turning the engine on…" : "Turning the engine off…";
            switch (EngineVisual)
            {
                case EngineVisual.Off:
                    return s.Recording ? "Off." : "Turn on to request video and recording access.";
                case EngineVisual.Standby:
                    return "Requests raw media and local-record permission when this node joins.";
                case EngineVisual.Moving:
                    return "Raw media is re-requested when the node arrives in the new room.";
                case EngineVisual.On:
                    return "Raw media: active" + (string.IsNullOrEmpty(s.RawMediaReason) ? "" : $" ({s.RawMediaReason})") +
                           (s.Recording ? ". Locked while recording." : "");
                case EngineVisual.Failed:
                    return $"Zoom refused raw media ({RawMediaRefusal(s)}). Ask the host to allow recording for this node " +
                           "(Participants > More > Allow record), then retry.";
                case EngineVisual.Requesting:
                    var since = _requestingSince is TimeSpan t ? $" · {Format.Elapsed(Now - t)}" : "";
                    var reason = s.RawMediaActive == false && !string.IsNullOrEmpty(s.RawMediaReason) ? $" Engine reports: {s.RawMediaReason}." : "";
                    return $"Requesting raw media and local-record permission from the host{since}.{reason}";
                default:
                    return "";
            }
        }
    }

    public IReadOnlyList<string> EngineWarnings => S?.EngineWarnings ?? [];
    public bool EngineWarningsVisible => EngineWarnings.Count > 0;
    public string EngineWarningsText => string.Join("\n", EngineWarnings);
    public string EngineButtonName => $"{EngineTitle}. {EngineDetail}";

    private async Task ToggleEngineAsync()
    {
        var target = S?.EngineOn != true;
        await SendEngineAsync(target);
    }

    private async Task RetryEngineAsync()
    {
        if (await SendEngineAsync(false)) await SendEngineAsync(true);
    }

    private async Task<bool> SendEngineAsync(bool on)
    {
        _engineError = null;
        _enginePendingTarget = on;
        RaiseAll();
        var result = await _main.SendAsync("engine", new { on });
        if (!result.Ok)
        {
            _engineError = result.Error;
            _enginePendingTarget = null;
        }
        else if (S?.EngineOn == on)
        {
            _enginePendingTarget = null;
        }
        RaiseAll();
        return result.Ok;
    }

    // ------------------------------------------------------------ plumbing

    public void Apply(EdgeState s)
    {
        if (_enginePendingTarget is bool target && s.EngineOn == target) _enginePendingTarget = null;
        if (!Joining) _joiningSince = null;
        else _joiningSince ??= Now;
        if (EngineVisual == EngineVisual.Requesting) _requestingSince ??= Now;
        else _requestingSince = null;
        if (s.InMeeting) _joinError = null;
        if (!s.Recording) _confirmLeave = false;
        RaiseAll();
    }

    public void Tick()
    {
        if (SignInPending || Joining || EngineVisual == EngineVisual.Requesting) RaiseAll();
    }

    private void RaiseAll()
    {
        foreach (var name in Names) Raise(name);
        SignInCommand.NotifyCanExecuteChanged();
        SignOutCommand.NotifyCanExecuteChanged();
        JoinCommand.NotifyCanExecuteChanged();
        JoinEndOtherCommand.NotifyCanExecuteChanged();
        LeaveCommand.NotifyCanExecuteChanged();
        ToggleEngineCommand.NotifyCanExecuteChanged();
        RetryEngineCommand.NotifyCanExecuteChanged();
    }

    private static readonly string[] Names =
    [
        nameof(SignedIn), nameof(SignInPending), nameof(SignInVisible), nameof(SignInTone), nameof(SignInDetail), nameof(SignInButtonText),
        nameof(JoinStepEnabled), nameof(Joining), nameof(Joined), nameof(CanJoin), nameof(MeetingFieldEnabled), nameof(JoinVisible),
        nameof(LeaveVisible), nameof(CanEndOther), nameof(ConfirmLeaveVisible), nameof(JoinButtonText), nameof(JoinTone),
        nameof(JoinDetail), nameof(JoinError), nameof(JoinErrorVisible),
        nameof(EngineEnabled), nameof(EnginePending), nameof(EngineVisual), nameof(EngineIsOn), nameof(EngineTone), nameof(EnginePulse),
        nameof(EngineTitle), nameof(EngineDetail), nameof(EngineWarnings), nameof(EngineWarningsVisible), nameof(EngineWarningsText),
        nameof(EngineButtonName),
    ];
}
