using System.Collections.ObjectModel;
using System.Runtime.InteropServices;
using System.Windows.Input;
using Avalonia;
using Avalonia.Media.Imaging;
using Avalonia.Platform;

namespace CoreVideoEdge.Desktop.ViewModels;

public enum TileHealth { Healthy, Warming, Degraded, Failed, NotPublishing, Departed, Moving, EngineOff, NoPreview }

// One routed slot on the Monitor. Every number is measured (preview header or
// node state); with nothing measured the tile says why instead of showing a value.
public sealed class TileViewModel : ObservableObject, IDisposable
{
    private const double DegradeRatio = 0.9;
    private static readonly TimeSpan DegradeAfter = TimeSpan.FromSeconds(3);
    private static readonly TimeSpan LostWindow = TimeSpan.FromSeconds(10);

    private readonly PreviewHeartbeatWatch _heartbeat = new();
    private PreviewFileReader? _reader;
    private string? _readerPath;
    private TimeSpan _nextOpenAttempt;
    private byte[]? _pixels;
    private uint _lastFrames;
    private long _lastFrameMs = -1;
    private PreviewHeader? _header;
    private string? _previewError;
    private TimeSpan? _belowTargetSince;
    private readonly Queue<(TimeSpan At, long Lost)> _lostHistory = new();

    private string _name = "";
    private WriteableBitmap? _bitmap;
    private TileHealth _health;

    public TileViewModel(int slot) => Slot = slot;

    public int Slot { get; }
    public string SlotTag => $"S{Slot}";
    public string Name { get => _name; private set => Set(ref _name, value); }
    public WriteableBitmap? Bitmap { get => _bitmap; private set => Set(ref _bitmap, value); }

    public TileHealth Health { get => _health; private set => Set(ref _health, value); }
    public bool HasPicture { get; private set; }
    public double PictureOpacity { get; private set; } = 1;
    public string Resolution { get; private set; } = "";
    public string FpsText { get; private set; } = "";
    public string LostText { get; private set; } = "";
    public bool LostRising { get; private set; }
    public bool LostVisible => LostText.Length > 0;
    public string DegradedText { get; private set; } = "";
    public bool DegradedVisible => DegradedText.Length > 0;
    public string FeedText { get; private set; } = "";
    public bool FeedVisible => FeedText.Length > 0;
    public Tone FeedTone { get; private set; }
    public string? FeedError { get; private set; }
    public bool FeedErrorVisible => !string.IsNullOrEmpty(FeedError);
    public bool IsoVisible { get; private set; }
    public bool IsoWriting { get; private set; }
    public string IsoText { get; private set; } = "";
    public double? AudioDb { get; private set; }
    public bool MeterVisible => AudioDb is not null && HasPicture && Health is TileHealth.Healthy or TileHealth.Degraded;
    public bool SlateVisible { get; private set; }
    public string SlateText { get; private set; } = "";
    public Tone SlateTone { get; private set; }
    public bool SlatePulse { get; private set; }
    public bool HeldVisible { get; private set; }
    public string HeldText { get; private set; } = "";
    public Tone BorderTone { get; private set; }
    public bool BorderWarn => BorderTone == Tone.Warn;
    public bool BorderError => BorderTone == Tone.Error;
    public string AccessibleName { get; private set; } = "";

    // Read the slot's preview file (at most 15 fps per tile; the caller paces).
    public void Poll(string? previewDir, TimeSpan now)
    {
        var path = previewDir is null ? null : PreviewFileReader.SlotPath(previewDir, Slot);
        if (path != _readerPath)
        {
            CloseReader();
            _readerPath = path;
            _nextOpenAttempt = TimeSpan.Zero;
        }
        if (path is null) return;
        if (_reader is null)
        {
            if (now < _nextOpenAttempt) return;
            _reader = PreviewFileReader.TryOpen(path, out _previewError);
            _nextOpenAttempt = now + TimeSpan.FromSeconds(1);
            if (_reader is null) return;
        }
        // The node deletes the file when the slot is unrouted; let go of it.
        if (!File.Exists(path))
        {
            CloseReader();
            return;
        }
        _pixels ??= new byte[480 * 4 * 270];
        PreviewRead read;
        try
        {
            var peek = _reader.TryRead(null);
            if (peek.Status != PreviewReadStatus.Ok) { _previewError = peek.Error; if (peek.Status == PreviewReadStatus.Invalid) CloseReader(); return; }
            var h = peek.Header!;
            var needed = h.Stride * h.Height;
            if (_pixels.Length != needed) _pixels = new byte[needed];
            var fresh = h.HasFrame && (h.Frames != _lastFrames || h.LastFrameMs != _lastFrameMs || _bitmap is null);
            read = fresh ? _reader.TryRead(_pixels) : peek;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ObjectDisposedException)
        {
            _previewError = ex.Message;
            CloseReader();
            return;
        }
        if (read.Status != PreviewReadStatus.Ok) return;
        _header = read.Header;
        _previewError = null;
        _heartbeat.Observe(_header!.HeartbeatMs, now);
        if (read.PixelsCopied)
        {
            CopyToBitmap(_header);
            _lastFrames = _header.Frames;
            _lastFrameMs = _header.LastFrameMs;
        }
    }

    private void CopyToBitmap(PreviewHeader h)
    {
        if (_bitmap is null || _bitmap.PixelSize.Width != h.Width || _bitmap.PixelSize.Height != h.Height)
            Bitmap = new WriteableBitmap(new PixelSize(h.Width, h.Height), new Vector(96, 96), PixelFormat.Bgra8888, AlphaFormat.Premul);
        using (var fb = _bitmap!.Lock())
        {
            for (var y = 0; y < h.Height; ++y)
                Marshal.Copy(_pixels!, y * h.Stride, fb.Address + y * fb.RowBytes, Math.Min(h.Width * 4, fb.RowBytes));
        }
        // Same instance, new pixels: tell the Image to redraw.
        Raise(nameof(Bitmap));
    }

    public void Update(EdgeState s, EdgeSubscription sub, TimeSpan now)
    {
        Name = sub.Name;
        var p = s.Participants?.FirstOrDefault(x => x.Id == sub.ParticipantId);
        var stream = s.Streams.LastOrDefault(x => x.Slot == Slot && x.ParticipantId is null || x.Slot == Slot && x.ParticipantId == sub.ParticipantId);
        var h = _header;
        var bound = h is not null && (h.ParticipantId == 0 || h.ParticipantId == sub.ParticipantId);

        HasPicture = bound && h!.HasFrame && _bitmap is not null;
        // Measured values: the preview first (what the node is drawing), else the node's state.
        var srcW = bound && h!.SourceWidth > 0 ? h.SourceWidth : p?.ReceivedWidth;
        var srcH = bound && h!.SourceHeight > 0 ? h.SourceHeight : p?.ReceivedHeight;
        var fps = bound && h!.Fps is double hf ? hf : p?.ReceivedFps;
        Resolution = srcW is int w && srcH is int hh ? Format.Size(w, hh) : "";
        FpsText = fps is double f ? Format.Fps(f) : "";
        AudioDb = bound ? h!.AudioDb : p?.AudioDb;

        // Lost: ISO arrivals overwritten or refused, cumulative for this take.
        if (stream is not null && (s.Recording || stream.State != "finalized"))
        {
            var lost = stream.VideoLost + (stream.VideoDropped ?? 0);
            LostText = $"lost {Format.Count(lost)}";
            _lostHistory.Enqueue((now, lost));
            while (_lostHistory.Count > 0 && now - _lostHistory.Peek().At > LostWindow) _lostHistory.Dequeue();
            LostRising = _lostHistory.Count > 1 && lost > _lostHistory.Peek().Lost;
        }
        else
        {
            LostText = "";
            LostRising = false;
            _lostHistory.Clear();
        }

        // Feed to Pro (measured by the node's sender): "NDI · 1 receiver",
        // "SRT · connected", or the node's error text verbatim.
        var feed = s.Feeds is { Mode: not "off" } feeds ? feeds.Slots.FirstOrDefault(x => x.Slot == Slot) : null;
        if (feed is null)
        {
            FeedText = "";
            FeedTone = Tone.Neutral;
            FeedError = null;
        }
        else
        {
            var kind = s.Feeds!.Mode == "ndi" ? "NDI" : "SRT";
            var detail = feed.State switch
            {
                "sending" when feed.Receivers is int rx => Format.Plural(rx, "receiver", "receivers"),
                "sending" when feed.Connected == true => "connected",
                "sending" => "sending",
                "waiting" when feed.Connected == false => "waiting for Pro",
                _ => feed.State,
            };
            FeedText = $"{kind} · {detail}";
            FeedTone = feed.State switch
            {
                "failed" => Tone.Error,
                "sending" when feed.Receivers is 0 => Tone.Warn,
                "sending" => Tone.Live,
                _ => Tone.Warn,
            };
            FeedError = feed.Error;
        }

        IsoVisible = sub.Iso;
        IsoWriting = sub.Iso && s.Recording && stream?.State is "recording" or "opening";
        IsoText = IsoWriting && s.RecordingElapsedMs is long ms ? $"ISO {Format.Clock(ms)}" : "ISO";

        // Degraded: under 90% of a constant target rate for 3 s (target known
        // only for 30/60 record formats), or frames lost in the last 10 s.
        var target = s.RecordFormat?.Fps is int t && t > 0 ? t : (int?)null;
        if (fps is double cur && target is int tgt && cur < tgt * DegradeRatio) _belowTargetSince ??= now;
        else _belowTargetSince = null;
        var fpsDegraded = _belowTargetSince is TimeSpan since && now - since >= DegradeAfter;
        DegradedText = fpsDegraded && fps is double c2 ? $"fps {c2:0} / {target}" : "";

        var frameAge = bound ? h!.FrameAgeMs : null;
        var failedIso = stream?.State == "failed";
        HeldVisible = false;
        HeldText = "";
        SlatePulse = false;
        PictureOpacity = 1;

        if (s.MovingRooms) Set(TileHealth.Moving, "Moving rooms. Guests re-attach when the new room's roster arrives.", Tone.Warn);
        else if (!sub.Present) Set(TileHealth.Departed, "Left the meeting. The slot is held for their return.", Tone.Warn);
        else if (s.EngineOn == false) Set(TileHealth.EngineOff, "Engine off. Turn it on in Connect to receive video.", Tone.Neutral);
        else if (failedIso) Set(TileHealth.Failed, $"ISO write failed: {stream!.Warning ?? "the writer stopped"}", Tone.Error);
        else if (s.PreviewDir is null && previewDirOverrideMissing)
            Set(TileHealth.NoPreview, "The node is not publishing previews (no preview folder in its state).", Tone.Neutral);
        else if (h is not null && _heartbeat.IsStalled(now))
            Set(TileHealth.NotPublishing, $"Preview not updating: the node has not published for {(int)_heartbeat.SinceChange(now).TotalSeconds} s.", Tone.Error);
        else if (h is null)
            Set(TileHealth.NoPreview, _previewError is null ? "Waiting for the node's preview." : $"No preview: {_previewError}", Tone.Neutral);
        else if (!h.HasFrame)
        {
            Set(TileHealth.Warming, "Waiting for first frame", Tone.Neutral);
            SlatePulse = true;
        }
        else if (frameAge > 2000)
        {
            Set(TileHealth.Failed, $"No video for {frameAge / 1000} s", Tone.Error);
            HeldVisible = HasPicture;
            HeldText = $"Last frame {frameAge / 1000} s ago";
            PictureOpacity = 0.4;
            SlateVisible = !HasPicture;
        }
        else if (fpsDegraded || LostRising) Set(TileHealth.Degraded, null, Tone.Warn);
        else Set(TileHealth.Healthy, null, Tone.Neutral);

        // No live picture: the last measured size/rate/level are not current,
        // so they are not shown (a frozen number never pretends to be live).
        if (Health is not TileHealth.Healthy and not TileHealth.Degraded)
        {
            Resolution = "";
            FpsText = "";
            AudioDb = null;
        }

        AccessibleName = $"{SlotTag}, {Name}, {HealthWords()}" +
                         (Resolution.Length > 0 ? $", {Resolution.Replace("×", " by ")}" : "") +
                         (FpsText.Length > 0 ? $", {FpsText.Replace("fps", "frames per second")}" : "") +
                         (IsoVisible ? IsoWriting ? ", ISO recording" : ", ISO armed" : "");
        RaiseAll();
    }

    // Set by the monitor: true when no preview folder is known at all.
    internal bool previewDirOverrideMissing;

    private void Set(TileHealth health, string? slate, Tone tone)
    {
        Health = health;
        SlateVisible = slate is not null && health is not TileHealth.Healthy and not TileHealth.Degraded;
        SlateText = slate ?? "";
        SlateTone = tone;
        BorderTone = health switch
        {
            TileHealth.Degraded or TileHealth.Departed or TileHealth.Moving => Tone.Warn,
            TileHealth.Failed or TileHealth.NotPublishing => Tone.Error,
            _ => Tone.Neutral,
        };
        // Pictures are only shown live; a held frame is shown dimmed by the caller.
        if (health is not TileHealth.Healthy and not TileHealth.Degraded) PictureOpacity = 0;
    }

    private string HealthWords() => Health switch
    {
        TileHealth.Healthy => "receiving",
        TileHealth.Warming => "waiting for first frame",
        TileHealth.Degraded => "degraded",
        TileHealth.Failed => "failed",
        TileHealth.NotPublishing => "preview not updating",
        TileHealth.Departed => "left the meeting",
        TileHealth.Moving => "moving rooms",
        TileHealth.EngineOff => "engine off",
        _ => "no preview",
    };

    private void RaiseAll()
    {
        foreach (var n in new[]
                 {
                     nameof(HasPicture), nameof(PictureOpacity), nameof(Resolution), nameof(FpsText), nameof(LostText),
                     nameof(LostRising), nameof(LostVisible), nameof(DegradedText), nameof(DegradedVisible), nameof(IsoVisible),
                     nameof(IsoWriting), nameof(IsoText), nameof(AudioDb), nameof(MeterVisible), nameof(SlateVisible),
                     nameof(SlateText), nameof(SlateTone), nameof(SlatePulse), nameof(HeldVisible), nameof(HeldText),
                     nameof(BorderTone), nameof(BorderWarn), nameof(BorderError), nameof(AccessibleName),
                     nameof(FeedText), nameof(FeedVisible), nameof(FeedTone), nameof(FeedError), nameof(FeedErrorVisible),
                 })
            Raise(n);
    }

    private void CloseReader()
    {
        _reader?.Dispose();
        _reader = null;
        _header = null;
        _heartbeat.Reset();
    }

    public void Dispose() => CloseReader();
}

// Monitor: one tile per routed slot, in slot order (a tile never jumps when
// another route changes). Unrouted slots are not drawn.
public sealed class MonitorViewModel : ObservableObject
{
    private readonly MainViewModel _main;
    private TileViewModel? _focused;

    public MonitorViewModel(MainViewModel main)
    {
        _main = main;
        OpenTileCommand = new RelayCommand(p => FocusedTile = p as TileViewModel);
        CloseTileCommand = new RelayCommand(() => FocusedTile = null);
    }

    public ObservableCollection<TileViewModel> Tiles { get; } = new();
    public ICommand OpenTileCommand { get; }
    public ICommand CloseTileCommand { get; }

    // The headless screenshot mode points the reader at copied preview files.
    public string? PreviewDirOverride { get; set; }

    public TileViewModel? FocusedTile
    {
        get => _focused;
        set { if (Set(ref _focused, value)) Raise(nameof(FocusVisible)); }
    }
    public bool FocusVisible => _focused is not null;

    public bool Empty => Tiles.Count == 0;
    public bool NotJoined => _main.State?.InMeeting != true && Tiles.Count == 0;
    public bool NothingRouted => _main.State?.InMeeting == true && Tiles.Count == 0;

    private string? PreviewDir => PreviewDirOverride ?? _main.State?.PreviewDir;

    public void Apply(EdgeState s)
    {
        var subs = s.Subscriptions.OrderBy(x => x.Slot).ToList();
        for (var i = Tiles.Count - 1; i >= 0; --i)
        {
            if (subs.Any(x => x.Slot == Tiles[i].Slot)) continue;
            if (_focused == Tiles[i]) FocusedTile = null;
            Tiles[i].Dispose();
            Tiles.RemoveAt(i);
        }
        for (var index = 0; index < subs.Count; ++index)
        {
            var sub = subs[index];
            var tile = Tiles.FirstOrDefault(t => t.Slot == sub.Slot);
            if (tile is null)
            {
                tile = new TileViewModel(sub.Slot);
                Tiles.Insert(index, tile);
            }
        }
        UpdateTiles();
        Raise(nameof(Empty));
        Raise(nameof(NotJoined));
        Raise(nameof(NothingRouted));
    }

    // Preview pacing: the view calls this at 15 Hz while the Monitor is shown.
    public void PollPreviews()
    {
        var now = _main.Clock.Now;
        foreach (var tile in Tiles) tile.Poll(PreviewDir, now);
        UpdateTiles();
    }

    private void UpdateTiles()
    {
        var s = _main.State;
        if (s is null) return;
        var now = _main.Clock.Now;
        foreach (var tile in Tiles)
        {
            var sub = s.Subscriptions.FirstOrDefault(x => x.Slot == tile.Slot);
            if (sub is null) continue;
            tile.previewDirOverrideMissing = PreviewDir is null;
            tile.Update(s, sub, now);
        }
    }
}
