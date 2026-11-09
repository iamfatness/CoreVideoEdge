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
    bool Receiving);

public sealed record EdgeSubscription(int Slot, uint ParticipantId, string Name, bool Present, bool Iso, bool FromConfig);

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
    string? Warning);

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
    public IReadOnlyList<string> ConfigErrors { get; init; } = [];
    public string? ConfigWarning { get; init; }
    public IReadOnlyList<string> EngineWarnings { get; init; } = [];
    public long? EngineSilentMs { get; init; }
    public bool? RawMediaActive { get; init; }
    public string? RawMediaReason { get; init; }
    public IReadOnlyList<EdgeParticipant>? Participants { get; init; }  // null: roster not known
    public IReadOnlyList<EdgeSubscription> Subscriptions { get; init; } = [];
    public bool Recording { get; init; }
    public long? RecordingElapsedMs { get; init; }
    public string? SessionDir { get; init; }
    public string? RecordingWarning { get; init; }
    public IReadOnlyList<EdgeIsoStream> Streams { get; init; } = [];

    public bool InMeeting => Phase == "in-meeting";
    public bool CanJoin => Phase is "idle" or "failed";

    public static EdgeState Parse(JsonElement root)
    {
        static string S(JsonElement e, string name, string fallback = "") =>
            e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? fallback : fallback;
        static string? SN(JsonElement e, string name) =>
            e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;
        static bool B(JsonElement e, string name) =>
            e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.True;
        static long? LN(JsonElement e, string name) =>
            e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number ? (long)v.GetDouble() : null;
        static long L(JsonElement e, string name) => LN(e, name) ?? 0;
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
                    hasReceived && B(r, "receiving"));
            }).ToList();
        }

        var subscriptions = root.TryGetProperty("subscriptions", out var subs) && subs.ValueKind == JsonValueKind.Array
            ? subs.EnumerateArray().Select(s => new EdgeSubscription(
                (int)L(s, "slot"), (uint)L(s, "participantId"), S(s, "name"), B(s, "present"), B(s, "iso"), B(s, "fromConfig"))).ToList()
            : new List<EdgeSubscription>();

        bool recording = false;
        long? elapsed = null;
        string? sessionDir = null, recordingWarning = null;
        var streams = new List<EdgeIsoStream>();
        if (root.TryGetProperty("recording", out var rec) && rec.ValueKind == JsonValueKind.Object)
        {
            recording = B(rec, "active");
            elapsed = LN(rec, "elapsedMs");
            sessionDir = SN(rec, "sessionDir");
            recordingWarning = SN(rec, "warning");
            if (rec.TryGetProperty("streams", out var list) && list.ValueKind == JsonValueKind.Array)
            {
                streams = list.EnumerateArray().Select(x => new EdgeIsoStream(
                    (int)L(x, "index"), (int)L(x, "slot"), S(x, "name"), S(x, "path"), S(x, "state"),
                    L(x, "videoFrames"), L(x, "videoLost"), L(x, "audioSamples"), L(x, "silenceSamples"),
                    L(x, "audioLostPackets"), SN(x, "encoder"), (int?)LN(x, "width"), (int?)LN(x, "height"),
                    LN(x, "msSinceVideo"), SN(x, "warning"))).ToList();
            }
        }

        bool? rawActive = null;
        string? rawReason = null;
        if (root.TryGetProperty("rawMedia", out var raw) && raw.ValueKind == JsonValueKind.Object)
        {
            rawActive = B(raw, "active");
            rawReason = SN(raw, "reason");
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
            ConfigErrors = Strings(root, "configErrors"),
            ConfigWarning = SN(root, "configWarning"),
            EngineWarnings = Strings(root, "engineWarnings"),
            EngineSilentMs = LN(root, "engineSilentMs"),
            RawMediaActive = rawActive,
            RawMediaReason = rawReason,
            Participants = participants,
            Subscriptions = subscriptions,
            Recording = recording,
            RecordingElapsedMs = elapsed,
            SessionDir = sessionDir,
            RecordingWarning = recordingWarning,
            Streams = streams,
        };
    }
}
