using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using CoreVideoPro.MediaCore.Services;
using CoreVideoPro.WinUI.Services;
using Microsoft.Win32;

namespace CoreVideoEdge.App;

public partial class MainWindow : Window
{
    private readonly bool _fake;
    private readonly EdgeNodeClient _node;
    private readonly ZoomOAuthService _oauth;
    private readonly string _configPath;
    private readonly CancellationTokenSource _closing = new();
    private readonly ObservableCollection<ParticipantRow> _participants = new();
    private readonly ObservableCollection<IsoRow> _isos = new();
    private EdgeState? _state;
    private bool _signedIn;
    private bool _busy;
    private string? _meetingCommandError;  // last refused meeting/source command
    private string? _recordCommandError;   // last refused record/folder command

    public MainWindow(bool fake, string? launchCallback)
    {
        InitializeComponent();
        _fake = fake;
        FakeBadge.Visibility = fake ? Visibility.Visible : Visibility.Collapsed;
        SourcesList.ItemsSource = _participants;
        HealthList.ItemsSource = _isos;

        _configPath = EdgeSettings.DefaultConfigPath;
        EdgeSettings.EnsureExists(_configPath);
        // CoreVideo Pro's Zoom OAuth service and DPAPI-protected token store,
        // in Edge's own per-user folder.
        _oauth = new ZoomOAuthService(new FileZoomTokenStore(
            Path.Combine(EdgeSettings.DataDirectory, "zoom-oauth.json"),
            DpapiSecretProtector.Protect,
            DpapiSecretProtector.Unprotect));

        _node = new EdgeNodeClient(Path.Combine(EdgeSettings.DataDirectory, "edge-node.log"));
        _node.StateChanged += state => Dispatcher.BeginInvoke(() => Apply(state));
        _node.Exited += message => Dispatcher.BeginInvoke(() =>
        {
            if (_closing.IsCancellationRequested) return;
            _meetingCommandError = message;
            Render();
            PhaseText.Text = "Edge node stopped";
        });

        Loaded += async (_, _) =>
        {
            _ = OAuthCallbackRelay.ServeAsync(url => Dispatcher.BeginInvoke(() => HandleOAuthCallback(url)), _closing.Token);
            try
            {
                _node.Start(ResolveNodeExecutable(), _configPath, _fake);
            }
            catch (Exception ex)
            {
                _meetingCommandError = ex.Message;
            }
            await RefreshAccountAsync();
            if (launchCallback is not null) HandleOAuthCallback(launchCallback);
        };
        Closing += OnClosing;
        Render();
    }

    private static string ResolveNodeExecutable()
    {
        var overridePath = Environment.GetEnvironmentVariable("COREVIDEO_EDGE_NODE");
        if (!string.IsNullOrWhiteSpace(overridePath)) return overridePath;
        var beside = Path.Combine(AppContext.BaseDirectory, "corevideo-edge.exe");
        if (File.Exists(beside)) return beside;
        // Development layout: <repo>/app/CoreVideoEdge.App/bin/... -> <repo>/build/bin.
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
        {
            var candidate = Path.Combine(dir.FullName, "build", "bin", "corevideo-edge.exe");
            if (File.Exists(candidate)) return candidate;
        }
        return beside;
    }

    // ---------------------------------------------------------------- account

    private async Task RefreshAccountAsync()
    {
        try
        {
            var status = await _oauth.GetStatusAsync();
            _signedIn = status.SignedIn;
            AccountText.Text = status.PendingAuthorization
                ? "Finish signing in in your browser."
                : _signedIn
                    ? "Signed in with Zoom. This node joins as that account."
                    : "Not signed in. Sign in with this node's own Zoom account (not CoreVideo Pro's) so joins do not collide.";
        }
        catch (Exception ex)
        {
            AccountText.Text = ex.Message;
        }
        Render();
    }

    private async void SignIn_Click(object sender, RoutedEventArgs e)
    {
        var exe = Environment.ProcessPath ?? Path.Combine(AppContext.BaseDirectory, "CoreVideoEdge.exe");
        if (!OAuthCallbackRelay.TryRegisterProtocolHandler(exe, out var error))
        {
            AccountText.Text = $"Could not register the corevideo:// sign-in return: {error}";
            return;
        }
        try
        {
            await _oauth.BeginAuthorizationAsync();
            AccountText.Text = "Finish signing in in your browser.";
        }
        catch (Exception ex)
        {
            AccountText.Text = ex.Message;
        }
    }

    private async void HandleOAuthCallback(string url)
    {
        try
        {
            await _oauth.HandleRedirectUrlAsync(url);
            Activate();
        }
        catch (Exception ex)
        {
            AccountText.Text = ex.Message;
            return;
        }
        await RefreshAccountAsync();
    }

    private async void SignOut_Click(object sender, RoutedEventArgs e)
    {
        await _oauth.SignOutAsync();
        await RefreshAccountAsync();
    }

    // ---------------------------------------------------------------- meeting

    private async Task JoinAsync(bool endOtherMeeting)
    {
        string? sdkJwt = null, userZak = null;
        if (_signedIn)
        {
            try
            {
                var credentials = await _oauth.EnsureJoinCredentialsAsync();
                sdkJwt = credentials.SdkJwt;
                userZak = credentials.UserZak;
            }
            catch (Exception ex)
            {
                _meetingCommandError = ex.Message;
                await RefreshAccountAsync();
                return;
            }
        }
        await RunAsync("join", new { meeting = MeetingBox.Text, sdkJwt, userZak, endOtherMeeting });
    }

    private async void Join_Click(object sender, RoutedEventArgs e) => await JoinAsync(false);
    private async void JoinEndOther_Click(object sender, RoutedEventArgs e) => await JoinAsync(true);
    private async void Leave_Click(object sender, RoutedEventArgs e) => await RunAsync("leave");

    // ---------------------------------------------------------------- sources

    private async void Subscribe_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not CheckBox { DataContext: ParticipantRow row } box) return;
        box.IsChecked = row.Subscribed;  // the node's state, not the click, decides
        await RunAsync("subscribe", new { participantId = row.Id, on = !row.Subscribed });
    }

    private async void Iso_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not CheckBox { DataContext: ParticipantRow row } box || row.Slot is null) return;
        box.IsChecked = row.Iso;
        await RunAsync("iso", new { slot = row.Slot, on = !row.Iso });
    }

    // ---------------------------------------------------------------- record

    private void Browse_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFolderDialog { Title = "Choose the ISO record folder" };
        if (Directory.Exists(FolderBox.Text)) dialog.InitialDirectory = FolderBox.Text;
        if (dialog.ShowDialog(this) == true)
        {
            FolderBox.Text = dialog.FolderName;
            _ = SaveFolderAsync();
        }
    }

    private async void Folder_LostFocus(object sender, RoutedEventArgs e) => await SaveFolderAsync();

    private async Task SaveFolderAsync()
    {
        if (_state is null || FolderBox.Text.Trim() == _state.RecordFolder) return;
        var path = FolderBox.Text.Trim();
        if (await RunAsync("set-record-folder", new { path })) TrySaveSettings(recordFolder: path);
    }

    private async void Prefix_LostFocus(object sender, RoutedEventArgs e)
    {
        if (_state is null || PrefixBox.Text.Trim() == _state.FilePrefix) return;
        var prefix = PrefixBox.Text.Trim();
        if (await RunAsync("set-file-prefix", new { prefix })) TrySaveSettings(filePrefix: prefix);
    }

    private void TrySaveSettings(string? recordFolder = null, string? filePrefix = null)
    {
        try
        {
            EdgeSettings.Update(_configPath, recordFolder, filePrefix);
        }
        catch (Exception ex)
        {
            _recordCommandError = ex.Message;
            Render();
        }
    }

    private async void Record_Click(object sender, RoutedEventArgs e)
    {
        await SaveFolderAsync();
        await RunAsync("record-start");
    }

    private async void Stop_Click(object sender, RoutedEventArgs e) => await RunAsync("record-stop");

    private void SessionLink_Click(object sender, RoutedEventArgs e)
    {
        var dir = _state?.SessionDir;
        if (!string.IsNullOrEmpty(dir) && Directory.Exists(dir))
            Process.Start(new ProcessStartInfo("explorer.exe", $"\"{dir}\"") { UseShellExecute = true });
    }

    // ---------------------------------------------------------------- plumbing

    private async Task<bool> RunAsync(string cmd, object? fields = null)
    {
        _busy = true;
        Render();
        try
        {
            var result = await _node.SendAsync(cmd, fields);
            var recordSide = cmd.StartsWith("record", StringComparison.Ordinal) || cmd.StartsWith("set-", StringComparison.Ordinal);
            if (recordSide) _recordCommandError = result.Ok ? null : result.Error;
            else _meetingCommandError = result.Ok ? null : result.Error;
            return result.Ok;
        }
        finally
        {
            _busy = false;
            Render();
        }
    }

    private void Apply(EdgeState state)
    {
        var first = _state is null;
        _state = state;
        if (first || !FolderBox.IsKeyboardFocusWithin) FolderBox.Text = state.RecordFolder;
        if (first || !PrefixBox.IsKeyboardFocusWithin) PrefixBox.Text = state.FilePrefix;

        var atCap = state.Subscriptions.Count >= state.MaxSubscriptions;
        var people = state.Participants ?? [];
        var byId = _participants.ToDictionary(r => r.Id);
        for (var i = _participants.Count - 1; i >= 0; --i)
            if (people.All(p => p.Id != _participants[i].Id)) _participants.RemoveAt(i);
        foreach (var p in people)
        {
            if (!byId.TryGetValue(p.Id, out var row))
            {
                row = new ParticipantRow(p.Id);
                _participants.Add(row);
            }
            row.Apply(p, state.Recording, atCap);
        }

        var streams = state.Streams;
        var byIndex = _isos.ToDictionary(r => r.Index);
        for (var i = _isos.Count - 1; i >= 0; --i)
            if (streams.All(s => s.Index != _isos[i].Index)) _isos.RemoveAt(i);
        foreach (var s in streams)
        {
            if (!byIndex.TryGetValue(s.Index, out var row))
            {
                row = new IsoRow(s.Index);
                _isos.Add(row);
            }
            row.Apply(s);
        }
        Render();
    }

    private void Render()
    {
        var s = _state;
        NodeNameText.Text = s is null ? "starting…" : $"{s.DisplayName}  ·  v{s.Version}";
        var phase = s?.Phase ?? "starting";
        PhaseText.Text = phase switch
        {
            "idle" => "Not in a meeting",
            "starting-engine" => "Starting the Zoom engine…",
            "authenticating" => "Authenticating the Meeting SDK…",
            "joining" => "Joining…",
            "in-meeting" => $"In meeting {s?.MeetingId}",
            "leaving" => "Leaving…",
            "failed" => "Join failed",
            _ => phase,
        };
        MeetingErrorText.Text = s?.Error ?? _meetingCommandError ?? "";

        var self = s?.Participants?.FirstOrDefault(p => p.Self);
        SelfText.Text = self is null
            ? ""
            : $"This node: mic {(self.Muted ? "muted" : "OPEN")}, camera {(self.VideoOn ? "ON" : "off")}";
        RawMediaText.Text = s?.RawMediaActive switch
        {
            true => "Zoom raw data: active",
            false => "Zoom raw data: not granted. Ask the host to allow this node to record (or make it co-host).",
            null => s?.InMeeting == true ? "Zoom raw data: waiting for Zoom" : "",
        };
        RawMediaText.Foreground = (System.Windows.Media.Brush)FindResource(s?.RawMediaActive == false ? "Warn" : "Muted");
        var warnings = new List<string>();
        if (s?.ConfigErrors.Count > 0) warnings.Add("Config: " + string.Join("; ", s.ConfigErrors));
        if (s?.ConfigWarning is not null) warnings.Add(s.ConfigWarning);
        if (s?.EngineWarnings.Count > 0) warnings.Add("Engine: " + string.Join("; ", s.EngineWarnings));
        if (s?.EngineSilentMs is long silent) warnings.Add($"The Zoom engine has been silent for {silent / 1000} s.");
        EngineWarningText.Text = string.Join("\n", warnings);

        var canJoin = s?.CanJoin == true && !_busy;
        JoinButton.IsEnabled = canJoin;
        MeetingBox.IsEnabled = canJoin;
        LeaveButton.IsEnabled = s is not null && !s.CanJoin && !_busy;
        EndOtherButton.Visibility = s?.CanEndOtherSession == true ? Visibility.Visible : Visibility.Collapsed;
        EndOtherButton.IsEnabled = canJoin;
        SignInButton.IsEnabled = !_signedIn;
        SignOutButton.IsEnabled = _signedIn && s?.CanJoin != false;

        var subscribed = s?.Subscriptions.Count ?? 0;
        SubscribedText.Text = s is null ? "" : $"{subscribed} of {s.MaxSubscriptions} subscribed at 1080p";
        SourcesHint.Text = s?.InMeeting != true
            ? "Join a meeting to see its participants."
            : subscribed == 0
                ? "Nothing is pulled from Zoom until you subscribe. Pick the guests CoreVideo Pro is not pulling."
                : "";
        var departed = s?.Subscriptions.Where(x => !x.Present).Select(x => x.Name).ToList() ?? [];
        if (departed.Count > 0) SourcesHint.Text = $"Left the meeting (slot kept for their return): {string.Join(", ", departed)}";

        var recording = s?.Recording == true;
        RecBadge.Visibility = recording ? Visibility.Visible : Visibility.Collapsed;
        var elapsed = TimeSpan.FromMilliseconds(s?.RecordingElapsedMs ?? 0);
        RecBadgeText.Text = $"REC {elapsed:hh\\:mm\\:ss}";
        var isoCount = s?.Subscriptions.Count(x => x.Iso && x.Present) ?? 0;
        RecordButton.IsEnabled = s?.InMeeting == true && !recording && !_busy && isoCount > 0;
        StopButton.IsEnabled = recording && !_busy;
        FolderBox.IsEnabled = !recording;
        PrefixBox.IsEnabled = !recording;
        BrowseButton.IsEnabled = !recording;
        RecordStatusText.Text = recording
            ? $"Recording {s!.Streams.Count} ISO(s) · {elapsed:hh\\:mm\\:ss}"
            : s?.InMeeting == true
                ? $"{isoCount} ISO(s) will be recorded."
                : "";
        RecordWarningText.Text = s?.RecordingWarning ?? _recordCommandError ?? "";
        SessionLinkText.Text = string.IsNullOrEmpty(s?.SessionDir) ? "" : s.SessionDir;
        HealthHeading.Text = recording ? "ISO HEALTH (live)" : s?.Streams.Count > 0 ? "ISO HEALTH (last take)" : "ISO HEALTH";
    }

    internal void RenderPreview(string statePath, string pngPath)
    {
        using var doc = System.Text.Json.JsonDocument.Parse(File.ReadAllText(statePath));
        _signedIn = true;
        AccountText.Text = "Signed in with Zoom. This node joins as that account.";
        Apply(EdgeState.Parse(doc.RootElement));
        var content = (FrameworkElement)Content;
        Content = null;
        var host = new Border { Background = (System.Windows.Media.Brush)FindResource("Ink"), Child = content };
        // Detached from the window, the content no longer inherits its Foreground.
        System.Windows.Documents.TextElement.SetForeground(host, Foreground);
        var size = new Size(Width, Height);
        host.Measure(size);
        host.Arrange(new Rect(size));
        host.UpdateLayout();
        var bitmap = new System.Windows.Media.Imaging.RenderTargetBitmap((int)Width, (int)Height, 96, 96,
            System.Windows.Media.PixelFormats.Pbgra32);
        bitmap.Render(host);
        var encoder = new System.Windows.Media.Imaging.PngBitmapEncoder();
        encoder.Frames.Add(System.Windows.Media.Imaging.BitmapFrame.Create(bitmap));
        using var file = File.Create(pngPath);
        encoder.Save(file);
    }

    private async void OnClosing(object? sender, System.ComponentModel.CancelEventArgs e)
    {
        if (_state?.Recording == true &&
            MessageBox.Show(this, "A take is recording. Stop it, finalize the ISO files and close?", "CoreVideo Edge",
                MessageBoxButton.OKCancel, MessageBoxImage.Warning) != MessageBoxResult.OK)
        {
            e.Cancel = true;
            return;
        }
        if (_closing.IsCancellationRequested) return;
        e.Cancel = true;  // finish the node shutdown (finalizes files) before the window goes
        _closing.Cancel();
        IsEnabled = false;
        PhaseText.Text = "Finalizing and shutting down…";
        await _node.DisposeAsync();
        Closing -= OnClosing;
        Close();
    }
}
