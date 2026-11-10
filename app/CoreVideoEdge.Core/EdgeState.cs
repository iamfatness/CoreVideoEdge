using System.Text.Json;

namespace CoreVideoEdge;

// Typed view of corevideo-edge's {"type":"state"} line (docs/PROTOCOL.md).
// Nullable members are values the node has not observed yet; the UI shows them
// as absent, never as a guess.
public sealed record EdgeParticipant(
    uint Id,
    string Name,
    bool VideoOn,
    bool Talking,
    bool Muted,
    bool Host,
    bool Self,
    int? Slot,
    bool Iso,
    int? ReceivedWidth,
    int? ReceivedHeight,
    bool Receiving,
    long? MsSinceFrame = null,
    double? ReceivedFps = null,      // v2: measured over the last second by the preview reader
    double? AudioDb = null);         // v2: isolated-audio peak dBFS over ~100 ms

// Resolution: the Zoom feed this slot asks for (1080/720/360). Zoom*: what the
// SDK's subscribe() accepted (the engine falls back a tier on refusal), absent
// until the engine reported it. Delivered size is on the participant.
public sealed record EdgeSubscription(int Slot, uint ParticipantId, string Name, bool Present, bool Iso, bool FromConfig,
    int Resolution = 1080, int? ZoomGranted = null, int? ZoomRefused = null, int? ZoomRefusedCode = null, bool ZoomFailed = false);

public sealed record EdgeIsoStream(
    int Index,
    int Slot,
    string Name,
    string Path,
    string State,
    long VideoFrames,
    long VideoLost,
    long AudioSamples,
    long SilenceSamples,
    long AudioLostPackets,
    string? Encoder,
    int? Width,
    int? Height,
    long? MsSinceVideo,
    string? Warning,
    uint? ParticipantId = null,
    long? VideoDropped = null,
    long? HeldRepeats = null,
    long? EncoderBacklogPeak = null,
    long? CadenceRepeats = null,     // v2: constant-rate formats only
    long? CadenceSkipped = null,     // v2
    long? MsSinceAudio = null,
    long? Bytes = null);             // present once the file is finalized

public sealed record EdgeRecordFormat(int Width, int Height, int Fps);

// Live feeds to CoreVideo Pro (docs/PROTOCOL.md "Feeds state"). The SRT
// passphrase is write-only: the state only says whether one is set.
public sealed record EdgeFeedSlot(
    int Slot,
    string Target,          // NDI source name, or the SRT URL with the passphrase redacted
    string State,           // starting | waiting | sending | failed
    long FramesSent,
    long FramesSkipped,
    double? AudioSeconds,
    int? Receivers,         // NDI: connected receivers, measured by the sender
    bool? Connected,        // SRT: Pro's listener reached
    string? Error);

public sealed record EdgeFeeds(
    string Mode,            // off | ndi | srt
    string? SrtHost,
    int? SrtBasePort,
    int? SrtLatencyMs,
    bool? SrtEncrypted,
    IReadOnlyList<EdgeFeedSlot> Slots);

public sealed record EdgeState
{
    public long Seq { get; init; }
    public string Version { get; init; } = "";
    public string EngineKind { get; init; } = "";
    public string Phase { get; init; } = "idle";
    public string DisplayName { get; init; } = "";
    public string? Error { get; init; }
    public bool CanEndOtherSession { get; init; }
    public string? MeetingId { get; init; }
    public int MaxSubscriptions { get; init; }
    public string RecordFolder { get; init; } = "";
    public string FilePrefix { get; init; } = "";
    public string? ConfigPath { get; init; }
    public IReadOnlyList<string> ConfigErrors { get; init; } = [];
    public string? ConfigWarning { get; init; }
    public IReadOnlyList<string> EngineWarnings { get; init; } = [];
    public long? EngineSilentMs { get; init; }
    public bool? RawMediaActive { get; init; }
    public string? RawMediaReason { get; init; }
    public bool? EngineOn { get; init; }                 // v2: the operator's Engine On choice
    public bool MovingRooms { get; init; }               // v2
    public EdgeRecordFormat? RecordFormat { get; init; } // v2
    public string? PreviewDir { get; init; }             // v2
    public EdgeFeeds? Feeds { get; init; }               // absent from nodes without feeds
    public IReadOnlyList<EdgeParticipant>? Participants { get; init; }  // null: roster not known
    public IReadOnlyList<EdgeSubscription> Subscriptions { get; init; } = [];
    public bool Recording { get; init; }
    public string? RecordingStartedUtc { get; init; }
    public long? RecordingElapsedMs { get; init; }
    public string? SessionDir { get; init; }
    public string? RecordingWarning { get; init; }
    public IReadOnlyList<EdgeIsoStream> Streams { get; init; } = [];

    public bool InMeeting => Phase == "in-meeting";
    public bool CanJoin => Phase is "idle" or "failed";

    public static EdgeState Parse(string line)
    {
        using var doc = JsonDocument.Parse(line);
        return Parse(doc.RootElement);
    }

    public static EdgeState Parse(JsonElement root)
    {
        static string S(JsonElement e, string name, string fallback = "") =>
            e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? fallback : fallback;
        static string? SN(JsonElement e, string name) =>
            e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;
        static bool B(JsonElement e, string name) =>
            e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.True;
        static bool? BN(JsonElement e, string name) =>
            e.TryGetProperty(name, out var v) && v.ValueKind is JsonValueKind.True or JsonValueKind.False
                ? v.ValueKind == JsonValueKind.True : null;
        static double? DN(JsonElement e, string name) =>
            e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number ? v.GetDouble() : null;
        static long? LN(JsonElement e, string name) => DN(e, name) is double d ? (long)d : null;
        static long L(JsonElement e, string name) => LN(e, name) ?? 0;
        static JsonElement? Zoom(JsonElement e) =>
            e.TryGetProperty("zoom", out var z) && z.ValueKind == JsonValueKind.Object ? z : null;
        static IReadOnlyList<string> Strings(JsonElement e, string name) =>
            e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Array
                ? v.EnumerateArray().Where(x => x.ValueKind == JsonValueKind.String).Select(x => x.GetString()!).ToList()
                : [];

        IReadOnlyList<EdgeParticipant>? participants = null;
        if (root.TryGetProperty("participants", out var people) && people.ValueKind == JsonValueKind.Array)
        {
            participants = people.EnumerateArray().Select(p =>
            {
                var hasReceived = p.TryGetProperty("received", out var r) && r.ValueKind == JsonValueKind.Object;
                return new EdgeParticipant(
                    (uint)L(p, "id"), S(p, "name"), B(p, "videoOn"), B(p, "talking"), B(p, "muted"), B(p, "host"),
                    B(p, "self"), (int?)LN(p, "slot"), B(p, "iso"),
                    hasReceived ? (int?)LN(r, "width") : null,
                    hasReceived ? (int?)LN(r, "height") : null,
                    hasReceived && B(r, "receiving"),
                    hasReceived ? LN(r, "msSinceFrame") : null,
                    hasReceived ? DN(r, "fps") : null,
                    hasReceived ? DN(r, "audioDb") : null);
            }).ToList();
        }

        var subscriptions = root.TryGetProperty("subscriptions", out var subs) && subs.ValueKind == JsonValueKind.Array
            ? subs.EnumerateArray().Select(s => new EdgeSubscription(
                (int)L(s, "slot"), (uint)L(s, "participantId"), S(s, "name"), B(s, "present"), B(s, "iso"), B(s, "fromConfig"),
                (int)(LN(s, "resolution") ?? 1080),
                Zoom(s) is { } z ? (int?)LN(z, "granted") : null,
                Zoom(s) is { } z2 ? (int?)LN(z2, "refused") : null,
                Zoom(s) is { } z3 ? (int?)LN(z3, "refusedCode") : null,
                Zoom(s) is { } z4 && B(z4, "failed"))).ToList()
            : new List<EdgeSubscription>();

        bool recording = false;
        long? elapsed = null;
        string? sessionDir = null, recordingWarning = null, startedUtc = null;
        var streams = new List<EdgeIsoStream>();
        if (root.TryGetProperty("recording", out var rec) && rec.ValueKind == JsonValueKind.Object)
        {
            recording = B(rec, "active");
            elapsed = LN(rec, "elapsedMs");
            startedUtc = SN(rec, "startedUtc");
            sessionDir = SN(rec, "sessionDir");
            recordingWarning = SN(rec, "warning");
            if (rec.TryGetProperty("streams", out var list) && list.ValueKind == JsonValueKind.Array)
            {
                streams = list.EnumerateArray().Select(x => new EdgeIsoStream(
                    (int)L(x, "index"), (int)L(x, "slot"), S(x, "name"), S(x, "path"), S(x, "state"),
                    L(x, "videoFrames"), L(x, "videoLost"), L(x, "audioSamples"), L(x, "silenceSamples"),
                    L(x, "audioLostPackets"), SN(x, "encoder"), (int?)LN(x, "width"), (int?)LN(x, "height"),
                    LN(x, "msSinceVideo"), SN(x, "warning"),
                    LN(x, "participantId") is long pid ? (uint)pid : null,
                    LN(x, "videoDropped"), LN(x, "heldRepeats"), LN(x, "encoderBacklogPeak"),
                    LN(x, "cadenceRepeats"), LN(x, "cadenceSkipped"), LN(x, "msSinceAudio"), LN(x, "bytes"))).ToList();
            }
        }

        bool? rawActive = null;
        string? rawReason = null;
        if (root.TryGetProperty("rawMedia", out var raw) && raw.ValueKind == JsonValueKind.Object)
        {
            rawActive = B(raw, "active");
            rawReason = SN(raw, "reason");
        }

        EdgeRecordFormat? format = null;
        if (root.TryGetProperty("recordFormat", out var fmt) && fmt.ValueKind == JsonValueKind.Object &&
            LN(fmt, "height") is long fh)
        {
            format = new EdgeRecordFormat((int)(LN(fmt, "width") ?? fh * 16 / 9), (int)fh, (int)(LN(fmt, "fps") ?? 0));
        }

        EdgeFeeds? feeds = null;
        if (root.TryGetProperty("feeds", out var fd) && fd.ValueKind == JsonValueKind.Object)
        {
            var slots = fd.TryGetProperty("slots", out var fs) && fs.ValueKind == JsonValueKind.Array
                ? fs.EnumerateArray().Select(x => new EdgeFeedSlot(
                    (int)L(x, "slot"), S(x, "target"), S(x, "state"), L(x, "framesSent"), L(x, "framesSkipped"),
                    DN(x, "audioSeconds"), (int?)LN(x, "receivers"), BN(x, "connected"), SN(x, "error"))).ToList()
                : new List<EdgeFeedSlot>();
            feeds = new EdgeFeeds(S(fd, "mode", "off"), SN(fd, "srtHost"), (int?)LN(fd, "srtBasePort"),
                (int?)LN(fd, "srtLatencyMs"), BN(fd, "srtEncrypted"), slots);
        }

        return new EdgeState
        {
            Seq = L(root, "seq"),
            Version = S(root, "version"),
            EngineKind = S(root, "engineKind"),
            Phase = S(root, "phase", "idle"),
            DisplayName = S(root, "displayName"),
            Error = SN(root, "error"),
            CanEndOtherSession = B(root, "canEndOtherSession"),
            MeetingId = SN(root, "meetingId"),
            MaxSubscriptions = (int)L(root, "maxSubscriptions"),
            RecordFolder = S(root, "recordFolder"),
            FilePrefix = S(root, "filePrefix"),
            ConfigPath = SN(root, "configPath"),
            ConfigErrors = Strings(root, "configErrors"),
            ConfigWarning = SN(root, "configWarning"),
            EngineWarnings = Strings(root, "engineWarnings"),
            EngineSilentMs = LN(root, "engineSilentMs"),
            RawMediaActive = rawActive,
            RawMediaReason = rawReason,
            EngineOn = BN(root, "engineOn"),
            MovingRooms = B(root, "movingRooms"),
            RecordFormat = format,
            PreviewDir = SN(root, "previewDir"),
            Feeds = feeds,
            Participants = participants,
            Subscriptions = subscriptions,
            Recording = recording,
            RecordingStartedUtc = startedUtc,
            RecordingElapsedMs = elapsed,
            SessionDir = sessionDir,
            RecordingWarning = recordingWarning,
            Streams = streams,
        };
    }
}
