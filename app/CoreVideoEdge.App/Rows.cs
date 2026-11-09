using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace CoreVideoEdge.App;

// Rows are updated IN PLACE from each node state (keyed by id), never rebuilt,
// so the lists stay stable while the node publishes several states a second.

public abstract class Row : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;

    protected void Set<T>(ref T field, T value, [CallerMemberName] string? name = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return;
        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
    }
}

public sealed class ParticipantRow : Row
{
    public ParticipantRow(uint id) => Id = id;
    public uint Id { get; }

    private string _name = "";
    private string _videoText = "";
    private string _talkingText = "";
    private string _receivedText = "";
    private bool _subscribed;
    private bool _iso;
    private bool _canSubscribe;
    private bool _canIso;
    private int? _slot;

    public string Name { get => _name; private set => Set(ref _name, value); }
    public string VideoText { get => _videoText; private set => Set(ref _videoText, value); }
    public string TalkingText { get => _talkingText; private set => Set(ref _talkingText, value); }
    public string ReceivedText { get => _receivedText; private set => Set(ref _receivedText, value); }
    public bool Subscribed { get => _subscribed; private set => Set(ref _subscribed, value); }
    public bool Iso { get => _iso; private set => Set(ref _iso, value); }
    public bool CanSubscribe { get => _canSubscribe; private set => Set(ref _canSubscribe, value); }
    public bool CanIso { get => _canIso; private set => Set(ref _canIso, value); }
    public int? Slot { get => _slot; private set => Set(ref _slot, value); }
    public string SubscribeLabel => $"Subscribe {Name}";
    public string IsoLabel => $"Record ISO for {Name}";

    public void Apply(EdgeParticipant p, bool recording, bool atCap)
    {
        Name = p.Self ? $"{p.Name} (this node)" : p.Host ? $"{p.Name} (host)" : p.Name;
        VideoText = p.VideoOn ? "on" : "off";
        TalkingText = p.Talking ? "talking" : "";
        Slot = p.Slot;
        Subscribed = p.Slot is not null;
        Iso = p.Slot is not null && p.Iso;
        // Received: only what the engine measured from frames that arrived.
        ReceivedText = p.Slot is null
            ? ""
            : p.ReceivedWidth is int w && p.ReceivedHeight is int h
                ? $"{w}×{h}{(p.Receiving ? "" : " (stalled)")}"
                : "waiting for video";
        CanSubscribe = !p.Self && (Subscribed ? !(recording && Iso) : !atCap);
        CanIso = Subscribed && !recording;
    }
}

public sealed class IsoRow : Row
{
    public IsoRow(int index) => Index = index;
    public int Index { get; }
    public string IndexText => $"{Index:00}";

    private string _name = "";
    private string _state = "";
    private string _framesText = "";
    private string _audioText = "";
    private string _lostText = "";
    private string _encoderText = "";
    private string _warning = "";

    public string Name { get => _name; private set => Set(ref _name, value); }
    public string State { get => _state; private set => Set(ref _state, value); }
    public string FramesText { get => _framesText; private set => Set(ref _framesText, value); }
    public string AudioText { get => _audioText; private set => Set(ref _audioText, value); }
    public string LostText { get => _lostText; private set => Set(ref _lostText, value); }
    public string EncoderText { get => _encoderText; private set => Set(ref _encoderText, value); }
    public string Warning { get => _warning; private set => Set(ref _warning, value); }

    public void Apply(EdgeIsoStream s)
    {
        Name = s.Name;
        State = s.State;
        FramesText = s.VideoFrames.ToString("N0");
        AudioText = $"{s.AudioSamples / 48000.0:0.0} s";
        LostText = $"{s.VideoLost:N0} / {s.AudioLostPackets:N0}";
        EncoderText = s.Encoder ?? "";
        Warning = s.Warning ?? "";
    }
}
