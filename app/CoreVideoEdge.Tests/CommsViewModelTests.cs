using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoEdge.Desktop.Services;
using CoreVideoEdge.Desktop.ViewModels;
using Xunit;

namespace CoreVideoEdge.Tests;

// Comms (ZComms talkback station in Edge): what each key sends, and that a
// cell only shows what the node's comms state says.
public sealed class CommsViewModelTests
{
    private sealed class RecordingNode : INodeChannel
    {
        public List<string> Sent { get; } = new();
        public Func<string, EdgeCommandResult> Reply { get; set; } = _ => new EdgeCommandResult(true, null);

        public Task<EdgeCommandResult> SendAsync(string cmd, object? fields = null)
        {
            var node = fields is null ? new JsonObject() : JsonSerializer.SerializeToNode(fields)!.AsObject();
            var ordered = new JsonObject { ["cmd"] = cmd };
            foreach (var (k, v) in node) ordered[k] = v?.DeepClone();
            Sent.Add(ordered.ToJsonString());
            return Task.FromResult(Reply(cmd));
        }
    }

    private sealed class FakeShell : IShellServices
    {
        public Task<string?> PickFolderAsync(string? start) => Task.FromResult<string?>(null);
        public void OpenFolder(string path) { }
        public long? FreeBytes(string path) => null;
        public string? VolumeName(string path) => null;
    }

    private static (CommsViewModel Vm, MainViewModel Main, RecordingNode Node) Make(string? state)
    {
        var node = new RecordingNode();
        var main = new MainViewModel(node, null, new FakeShell(), new FixedClock(TimeSpan.FromSeconds(100)),
            new AppOptions { JoinWithoutSignIn = true });
        if (state is not null) main.Apply(EdgeState.Parse(state));
        return (main.Comms, main, node);
    }

    private static string Channel(int slot, string? label = null, int listeners = 0, bool ready = false, bool keyed = false,
        bool latched = false, string? room = null, int reachOk = 0, int reachDark = 0) =>
        $$"""{"slot":{{slot}},"label":{{(label is null ? "null" : $"\"{label}\"")}},"listeners":{{listeners}},"ready":{{(ready ? "true" : "false")}},"keyed":{{(keyed ? "true" : "false")}},"latched":{{(latched ? "true" : "false")}},"room":{{(room is null ? "null" : $"\"{room}\"")}},"reachOk":{{reachOk}},"reachDark":{{reachDark}}}""";

    private static string State(string channels, string roster = "", bool available = true, string? reason = null,
        double? peak = -24, string mic = "", string phase = "in-meeting") => $$$"""
        {"type":"state","phase":"{{{phase}}}","engineKind":"fake","maxSubscriptions":8,"meetingId":"81234567890",
         "subscriptions":[],"recording":{"active":false,"streams":[]},
         "comms":{"available":{{{(available ? "true" : "false")}}},"reason":{{{(reason is null ? "null" : $"\"{reason}\"")}}},
           "lamps":{"link":true,"meetingMic":true,"channel":true,"tx":false},"room":null,
           "channels":[{{{channels}}}],
           "roster":[{{{roster}}}],
           "mic":{"device":"Headset","devices":["Headset","USB Mic"],"peakDbfs":{{{(peak is double p ? p.ToString(System.Globalization.CultureInfo.InvariantCulture) : "null")}}},"gainDb":0,
                  "sidetone":true,"sidetoneDevice":"Headphones","sidetoneDevices":["Headphones","Speakers"],"tone":false{{{mic}}}},
           "stats":{"framesSent":1200,"channelSends":800,"underruns":0,"sendErrors":0},
           "log":["bank up: 16 channels","Ann joined CH 1"]}}
        """;

    private static readonly string Two = string.Join(",",
        Channel(0, "Ann", 1, true, reachOk: 1),
        Channel(1, "Bo", 1, true, reachOk: 1),
        Channel(2),
        Channel(3));

    private const string People = """
        {"participantId":101,"name":"Ann","talkback":true,"room":null,"channels":[0]},
        {"participantId":102,"name":"Bo","talkback":true,"room":null,"channels":[1]},
        {"participantId":103,"name":"Cy","talkback":false,"room":null,"channels":[]}
        """;

    [Fact]
    public void ParsesTheCommsObject()
    {
        var s = EdgeState.Parse(State(Two, People));
        Assert.NotNull(s.Comms);
        Assert.True(s.Comms!.Available);
        Assert.Equal(4, s.Comms.Channels.Count);
        Assert.Equal("Ann", s.Comms.Channels[0].Label);
        Assert.Equal(3, s.Comms.Roster.Count);
        Assert.False(s.Comms.Roster[2].Talkback);
        Assert.Equal(new[] { 0 }, s.Comms.Roster[0].Channels);
        Assert.Equal(-24, s.Comms.Mic.PeakDbfs);
        Assert.Equal(1200, s.Comms.FramesSent);
        Assert.Equal("Ann joined CH 1", s.Comms.Log[^1]);
    }

    [Fact]
    public void AbsentCommsMeansWaitingNotUnavailable()
    {
        var (vm, _, _) = Make("""{"type":"state","phase":"in-meeting","subscriptions":[],"recording":{"active":false,"streams":[]}}""");
        Assert.True(vm.Waiting);
        Assert.False(vm.DeskVisible);
        Assert.False(vm.LampLink);
    }

    [Fact]
    public void NotHostSaysWhyAndHowToFixIt()
    {
        var (vm, _, _) = Make(State("", available: false, reason: "This account is not host or co-host."));
        Assert.True(vm.Unavailable);
        Assert.Equal("This account is not host or co-host.", vm.UnavailableText);
        Assert.True(vm.HostHintVisible);
        Assert.False(vm.DeskVisible);
    }

    [Fact]
    public void OnlyOccupiedChannelsAreCells()
    {
        var (vm, _, _) = Make(State(Two, People));
        Assert.Equal(new[] { "Ann", "Bo" }, vm.Cells.Select(c => c.Name));
        Assert.All(vm.Cells, c => Assert.Equal("ready", c.StateText));
        Assert.Equal("1", vm.Cells[0].KeyHint);
    }

    [Fact]
    public async Task HoldingACellKeysItAndReleasingUnkeys()
    {
        var (vm, _, node) = Make(State(Two, People));
        var ann = vm.Cells[0];
        await vm.PressAsync(ann);
        await vm.PressAsync(ann);   // a second down while held sends nothing
        await vm.ReleaseAsync(ann);
        await vm.ReleaseAsync(ann); // and a second up nothing
        Assert.Equal(new[]
        {
            """{"cmd":"comms-key","slot":0,"on":true}""",
            """{"cmd":"comms-key","slot":0,"on":false}""",
        }, node.Sent);
        Assert.False(ann.Hot); // the node has not said keyed
    }

    [Fact]
    public void OnAirComesFromTheNodeOnly()
    {
        var keyed = string.Join(",", Channel(0, "Ann", 1, true, keyed: true, reachOk: 1), Channel(1, "Bo", 0, true, keyed: true));
        var (vm, _, _) = Make(State(keyed, People));
        Assert.True(vm.Cells[0].Hot);
        Assert.Equal("on air", vm.Cells[0].StateText);
        Assert.True(vm.Cells[1].Armed);   // keyed, nobody listening
        Assert.Equal("nobody in channel", vm.Cells[1].StateText);
    }

    [Fact]
    public async Task LatchModeTogglesInsteadOfHolding()
    {
        var (vm, main, node) = Make(State(Two, People));
        vm.LatchMode = true;
        await vm.PressAsync(vm.Cells[0]);
        main.Apply(EdgeState.Parse(State(string.Join(",", Channel(0, "Ann", 1, true, keyed: true, latched: true, reachOk: 1),
            Channel(1, "Bo", 1, true, reachOk: 1)), People)));
        Assert.Equal("on air · latched", vm.Cells[0].StateText);
        await vm.PressAsync(vm.Cells[0]);
        Assert.Equal(new[]
        {
            """{"cmd":"comms-latch","slot":0,"on":true}""",
            """{"cmd":"comms-latch","slot":0,"on":false}""",
        }, node.Sent);
    }

    [Fact]
    public async Task AllCallKeysEveryChannel()
    {
        var (vm, _, node) = Make(State(Two, People));
        await vm.AllPressAsync();
        await vm.AllReleaseAsync();
        Assert.Equal(new[]
        {
            """{"cmd":"comms-key","all":true,"on":true}""",
            """{"cmd":"comms-key","all":true,"on":false}""",
        }, node.Sent);
    }

    [Fact]
    public void AnUpdateThatKeepsTheSameCellsNeverRebuildsThem()
    {
        // ZComms law: rebuilding the grid under a press eats the release.
        var (vm, main, _) = Make(State(Two, People));
        var before = vm.Cells[0];
        main.Apply(EdgeState.Parse(State(string.Join(",", Channel(0, "Ann", 1, true, keyed: true, reachOk: 1),
            Channel(1, "Bo", 1, true, reachOk: 1)), People, peak: -6)));
        Assert.Same(before, vm.Cells[0]);
        Assert.True(before.Hot);
    }

    [Fact]
    public async Task LeavingTheScreenReleasesAHeldKey()
    {
        var (vm, main, node) = Make(State(Two, People));
        main.SelectedScreen = Screen.Comms;
        await vm.PressAsync(vm.Cells[1]);
        main.SelectedScreen = Screen.Routing;
        await vm.LastSend;
        Assert.Equal("""{"cmd":"comms-key","slot":1,"on":false}""", node.Sent[^1]);
    }

    [Fact]
    public void SomeoneInAnotherRoomCannotBeKeyed()
    {
        var chans = string.Join(",", Channel(0, "Ann", 1, true, room: "Room 2"), Channel(1, "Bo", 1, true, reachOk: 1));
        var (vm, _, _) = Make(State(chans, People));
        Assert.True(vm.Cells[0].Off);
        Assert.Equal("in Room 2", vm.Cells[0].StateText);
        Assert.Null(vm.Cells[0].KeyHint);
        Assert.Same(vm.Cells[1], vm.CellForDigit(1)); // digits skip the unreachable cell
    }

    [Fact]
    public void AGroupSaysHowManyListenAndWhoIsElsewhere()
    {
        var (vm, _, _) = Make(State(Channel(4, null, 3, true, reachOk: 3, reachDark: 1), People));
        Assert.Equal("CH 5", vm.Cells[0].Name);
        Assert.Equal("3 listening · 1 elsewhere", vm.Cells[0].StateText);
    }

    [Fact]
    public async Task EditTalentChipsAssignAndExplainWebClients()
    {
        var (vm, _, node) = Make(State(Two, People));
        vm.EditMode = true;
        Assert.True(vm.EditVisible);
        Assert.False(vm.GridVisible);
        var ann = vm.People[0];
        Assert.Equal(4, ann.Chips.Count);                 // every channel, not a subset
        Assert.True(ann.Chips[0].On);
        Assert.True(ann.Chips[1].Busy);                    // Bo is on CH 2
        Assert.Contains("with Bo", ann.Chips[1].Tip);
        Assert.True(vm.People[2].NoTalkback);
        Assert.Empty(vm.People[2].Chips);

        ann.Chips[1].Command.Execute(null);
        await vm.LastSend;
        ann.Chips[0].Command.Execute(null);
        await vm.LastSend;
        Assert.Equal(new[]
        {
            """{"cmd":"comms-assign","slot":1,"participantId":101,"on":true}""",
            """{"cmd":"comms-assign","slot":0,"participantId":101,"on":false}""",
        }, node.Sent);
    }

    [Fact]
    public async Task SettingsSendTheirVerbs()
    {
        var (vm, _, node) = Make(State(Two, People));
        vm.SelectedMic = "USB Mic";
        await vm.LastSend;
        vm.SelectedOutput = "Speakers";
        await vm.LastSend;
        vm.SidetoneCommand.Execute(null);
        await vm.LastSend;
        vm.ToneCommand.Execute(null);
        await vm.LastSend;
        vm.GainUpCommand.Execute(null);
        await vm.LastSend;
        Assert.Equal(new[]
        {
            """{"cmd":"comms-mic","device":"USB Mic"}""",
            """{"cmd":"comms-sidetone-out","device":"Speakers"}""",
            """{"cmd":"comms-sidetone","on":false}""",
            """{"cmd":"comms-tone","on":true}""",
            """{"cmd":"comms-gain","db":1}""",
        }, node.Sent);
    }

    [Fact]
    public void ApplyingStateNeverSendsADeviceChange()
    {
        var (_, main, node) = Make(State(Two, People));
        main.Apply(EdgeState.Parse(State(Two, People)));
        Assert.Empty(node.Sent);
    }

    [Fact]
    public async Task ARefusalIsShownVerbatim()
    {
        var (vm, _, node) = Make(State(Two, People));
        node.Reply = _ => new EdgeCommandResult(false, "Not in a meeting.");
        await vm.PressAsync(vm.Cells[0]);
        Assert.Equal("Not in a meeting.", vm.Error);
    }

    [Fact]
    public void MeterLightsOnlyFromAMeasuredLevel()
    {
        var (vm, main, _) = Make(State(Two, People, peak: -24));
        Assert.Equal(6, vm.Meter.Count(s => s.Lit));       // -24 dBFS on -48..0 = half
        main.Apply(EdgeState.Parse(State(Two, People, peak: null)));
        Assert.Equal(0, vm.Meter.Count(s => s.Lit));
    }

    [Fact]
    public void OpsStripShowsTheLatestLineAndHistoryNewestFirst()
    {
        var (vm, _, _) = Make(State(Two, People));
        Assert.Equal("Ann joined CH 1", vm.OpsLine);
        Assert.Equal("Ann joined CH 1", vm.OpsLines[0]);
    }
}
