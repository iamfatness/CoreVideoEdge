using System.Text.Json;
using System.Text.Json.Nodes;
using CoreVideoEdge.Desktop.Services;
using CoreVideoEdge.Desktop.ViewModels;
using Xunit;

namespace CoreVideoEdge.Tests;

// What JSON each operator action sends to the node, and that the UI only
// reflects what the node's state confirms.
public sealed class ViewModelCommandTests
{
    // Serializes exactly like EdgeNodeClient.SendAsync (minus the id).
    private sealed class RecordingNode : INodeChannel
    {
        public List<string> Sent { get; } = new();
        public Func<string, EdgeCommandResult> Reply { get; set; } = _ => new EdgeCommandResult(true, null);

        public Task<EdgeCommandResult> SendAsync(string cmd, object? fields = null)
        {
            var node = fields is null ? new JsonObject() : JsonSerializer.SerializeToNode(fields)!.AsObject();
            node["cmd"] = cmd;
            var ordered = new JsonObject { ["cmd"] = cmd };
            foreach (var (k, v) in node) if (k != "cmd") ordered[k] = v?.DeepClone();
            Sent.Add(ordered.ToJsonString());
            return Task.FromResult(Reply(cmd));
        }
    }

    private sealed class FakeShell : IShellServices
    {
        public string? Picked { get; set; }
        public Task<string?> PickFolderAsync(string? start) => Task.FromResult(Picked);
        public void OpenFolder(string path) { }
        public long? FreeBytes(string path) => null;
        public string? VolumeName(string path) => null;
    }

    private static (MainViewModel Vm, RecordingNode Node, FixedClock Clock) Make(string? state = null)
    {
        var node = new RecordingNode();
        var clock = new FixedClock(TimeSpan.FromSeconds(100));
        var vm = new MainViewModel(node, null, new FakeShell(), clock, new AppOptions { JoinWithoutSignIn = true });
        if (state is not null) vm.Apply(EdgeState.Parse(state));
        return (vm, node, clock);
    }

    private const string InMeeting = """
        {"type":"state","phase":"in-meeting","engineKind":"fake","engineOn":true,"maxSubscriptions":8,"meetingId":"81234567890",
         "recordFolder":"D:\\EdgeISO","recordFormat":{"width":1920,"height":1080,"fps":0},"rawMedia":{"active":true,"reason":"fake"},
         "participants":[{"id":101,"name":"Ann","videoOn":true,"slot":1,"iso":true,
                           "received":{"width":1920,"height":1080,"msSinceFrame":20,"receiving":true}},
                         {"id":102,"name":"Bo","videoOn":true},
                         {"id":103,"name":"Cy","videoOn":false},
                         {"id":100,"name":"Edge","videoOn":false,"self":true}],
         "subscriptions":[{"slot":1,"participantId":101,"name":"Ann","present":true,"iso":true,"fromConfig":false}],
         "feeds":{"mode":"off","slots":[]},
         "recording":{"active":false,"streams":[]}}
        """;

    private static string Recording => InMeeting.Replace("\"recording\":{\"active\":false,\"streams\":[]}",
        "\"recording\":{\"active\":true,\"elapsedMs\":5000,\"streams\":[{\"index\":1,\"slot\":1,\"name\":\"Ann\",\"participantId\":101,\"path\":\"D:\\\\EdgeISO\\\\x\\\\ISO-01-Ann.mp4\",\"state\":\"recording\",\"videoFrames\":150,\"videoLost\":0,\"audioSamples\":240000,\"silenceSamples\":0,\"audioLostPackets\":0}]}");

    [Fact]
    public async Task EngineToggleSendsTheOppositeOfTheNodeState()
    {
        var (vm, node, _) = Make(InMeeting);
        await vm.Connect.ToggleEngineCommand.ExecuteAsync();
        Assert.Equal("""{"cmd":"engine","on":false}""", node.Sent.Single());
        Assert.True(vm.Connect.EnginePending);                // until the state confirms

        vm.Apply(EdgeState.Parse(InMeeting.Replace("\"engineOn\":true", "\"engineOn\":false")));
        Assert.False(vm.Connect.EnginePending);
        Assert.Equal(EngineVisual.Off, vm.Connect.EngineVisual);
        await vm.Connect.ToggleEngineCommand.ExecuteAsync();
        Assert.Equal("""{"cmd":"engine","on":true}""", node.Sent[1]);
    }

    [Fact]
    public void EngineStatesFollowTheNode()
    {
        var (vm, _, _) = Make(InMeeting);
        Assert.Equal(EngineVisual.On, vm.Connect.EngineVisual);
        vm.Apply(EdgeState.Parse(InMeeting.Replace("\"rawMedia\":{\"active\":true,\"reason\":\"fake\"},", "")));
        Assert.Equal(EngineVisual.Requesting, vm.Connect.EngineVisual);   // on, nothing granted yet
        vm.Apply(EdgeState.Parse(InMeeting.Replace("\"phase\":\"in-meeting\"", "\"phase\":\"in-meeting\",\"movingRooms\":true")));
        Assert.Equal(EngineVisual.Moving, vm.Connect.EngineVisual);
        vm.Apply(EdgeState.Parse("""{"type":"state","phase":"idle","engineOn":true}"""));
        Assert.Equal(EngineVisual.Standby, vm.Connect.EngineVisual);
        vm.Apply(EdgeState.Parse("""{"type":"state","phase":"idle"}"""));
        Assert.Equal(EngineVisual.Unknown, vm.Connect.EngineVisual);     // not reported: not "off"
    }

    [Fact]
    public async Task JoinSendsTheTrimmedLink()
    {
        var (vm, node, _) = Make("""{"type":"state","phase":"idle","engineOn":true,"maxSubscriptions":8}""");
        vm.Connect.MeetingLink = "  https://zoom.us/j/81234567890?pwd=abc ";
        await vm.Connect.JoinCommand.ExecuteAsync();
        Assert.Equal("""{"cmd":"join","meeting":"https://zoom.us/j/81234567890?pwd=abc","sdkJwt":null,"userZak":null,"endOtherMeeting":false}""",
            node.Sent.Single());
    }

    [Fact]
    public async Task JoinRefusalIsShownAtTheField()
    {
        var (vm, node, _) = Make("""{"type":"state","phase":"idle","engineOn":true}""");
        node.Reply = _ => new EdgeCommandResult(false, "Zoom rejected the meeting passcode. Use the full join link with its pwd=.");
        vm.Connect.MeetingLink = "81234567890";
        await vm.Connect.JoinCommand.ExecuteAsync();
        Assert.Equal("Zoom rejected the meeting passcode. Use the full join link with its pwd=.", vm.Connect.JoinError);
    }

    [Fact]
    public async Task LeaveWhileRecordingAsksFirst()
    {
        var (vm, node, _) = Make(Recording);
        await vm.Connect.LeaveCommand.ExecuteAsync();
        Assert.Empty(node.Sent);
        Assert.True(vm.Connect.ConfirmLeaveVisible);
        await vm.Connect.ConfirmLeaveCommand.ExecuteAsync();
        Assert.Equal("""{"cmd":"leave"}""", node.Sent.Single());
    }

    [Fact]
    public async Task SubscribeCellRoutesToThatSlotAndStaysPendingUntilConfirmed()
    {
        var (vm, node, _) = Make(InMeeting);
        var bo = vm.Routing.Rows.Single(r => r.Name == "Bo");
        var cell = bo.SubscribeCells[4];                       // the operator picks S5
        await vm.Routing.ToggleAsync(cell);
        Assert.Equal("""{"cmd":"subscribe","participantId":102,"on":true,"slot":5,"resolution":1080,"fps":0}""", node.Sent.Single());
        // The pending mark sits on the crosspoint that was picked, never "routed" yet.
        Assert.Equal(CellState.Pending, bo.SubscribeCells[4].State);
        Assert.DoesNotContain(bo.SubscribeCells, c => c.State == CellState.Routed);

        vm.Apply(EdgeState.Parse(InMeeting
            .Replace("{\"id\":102,\"name\":\"Bo\",\"videoOn\":true}", "{\"id\":102,\"name\":\"Bo\",\"videoOn\":true,\"slot\":5,\"iso\":false}")
            .Replace("\"subscriptions\":[", "\"subscriptions\":[{\"slot\":5,\"participantId\":102,\"name\":\"Bo\",\"present\":true,\"iso\":false,\"fromConfig\":false},")));
        Assert.Equal(CellState.Routed, bo.SubscribeCells[4].State);
        // The other crosspoints stay drawn (owner 2026-11-09): never hidden.
        Assert.DoesNotContain(bo.SubscribeCells, c => c.State == CellState.Hidden);
        Assert.Equal(CellState.Empty, bo.SubscribeCells[1].State);         // still live: a click moves Bo
    }

    [Fact]
    public void NoCrosspointIsEverHidden()
    {
        var (vm, _, _) = Make(InMeeting);
        foreach (var row in vm.Routing.Rows)
        {
            Assert.Equal(8, row.SubscribeCells.Count);
            Assert.DoesNotContain(row.SubscribeCells.Concat(row.IsoCells), c => !c.IsVisible);
        }
        var bo = vm.Routing.Rows.Single(r => r.Name == "Bo");
        Assert.Equal(CellState.Empty, bo.SubscribeCells[0].State);         // S1 carries Ann: a click replaces her
        Assert.Contains("Ann", bo.SubscribeCells[0].Tip);
        Assert.Equal(CellState.Empty, bo.SubscribeCells[7].State);
    }

    [Fact]
    public async Task FeedResolutionDefaultGoesWithNewRoutes()
    {
        var (vm, node, _) = Make(InMeeting);
        vm.Routing.DefaultResolution = 720;
        var bo = vm.Routing.Rows.Single(r => r.Name == "Bo");
        await vm.Routing.ToggleAsync(bo.SubscribeCells[1]);
        Assert.Equal("""{"cmd":"subscribe","participantId":102,"on":true,"slot":2,"resolution":720,"fps":0}""", node.Sent.Single());
    }

    [Fact]
    public async Task ChangingARoutedSlotsResolutionSendsSetResolution()
    {
        var (vm, node, _) = Make(InMeeting);
        var ann = vm.Routing.Rows.Single(r => r.Name == "Ann");
        Assert.Equal(1080, ann.Resolution);
        ann.Resolution = 720;
        await vm.Routing.LastResolutionSend!;
        Assert.Equal("""{"cmd":"set-resolution","slot":1,"resolution":720}""", node.Sent.Single());
        // A state line echoing the node's value never sends again.
        vm.Apply(EdgeState.Parse(InMeeting.Replace("\"fromConfig\":false}]", "\"fromConfig\":false,\"resolution\":720}]")));
        Assert.Equal(720, ann.Resolution);
        Assert.Single(node.Sent);
    }

    [Fact]
    public void DeliveredBelowRequestedIsSaidPlainly()
    {
        var (vm, _, _) = Make(InMeeting
            .Replace("\"received\":{\"width\":1920,\"height\":1080", "\"received\":{\"width\":1280,\"height\":720")
            .Replace("\"fromConfig\":false}]", "\"fromConfig\":false,\"resolution\":1080,\"zoom\":{\"granted\":1080}}]"));
        var ann = vm.Routing.Rows.Single(r => r.Name == "Ann");
        Assert.True(ann.OfferedBelowAsked);
        Assert.Contains("Zoom accepted 1080p", ann.OfferedTip);
        Assert.Contains("1280×720", ann.OfferedTip);
    }

    [Fact]
    public void AnSdkRefusalOf1080IsShownOnTheRow()
    {
        var (vm, _, _) = Make(InMeeting
            .Replace("\"fromConfig\":false}]", "\"fromConfig\":false,\"resolution\":1080,\"zoom\":{\"granted\":720,\"refused\":1080,\"refusedCode\":12}}]"));
        var ann = vm.Routing.Rows.Single(r => r.Name == "Ann");
        Assert.Contains("refused 1080p", ann.Detail);
        Assert.Contains("12", ann.Detail);
    }

    [Fact]
    public async Task RoutedCellUnsubscribes()
    {
        var (vm, node, _) = Make(InMeeting);
        var ann = vm.Routing.Rows.Single(r => r.Name == "Ann");
        Assert.Equal(CellState.Routed, ann.SubscribeCells[0].State);
        await vm.Routing.ToggleAsync(ann.SubscribeCells[0]);
        Assert.Equal("""{"cmd":"subscribe","participantId":101,"on":false}""", node.Sent.Single());
    }

    [Fact]
    public async Task RefusedSubscribeShowsTheNodesReason()
    {
        var (vm, node, _) = Make(InMeeting);
        node.Reply = _ => new EdgeCommandResult(false, "All 8 subscription slots are in use.");
        var bo = vm.Routing.Rows.Single(r => r.Name == "Bo");
        await vm.Routing.ToggleAsync(bo.SubscribeCells[1]);
        Assert.Equal("All 8 subscription slots are in use.", bo.Detail);
        Assert.True(bo.DetailIsError);
        Assert.Contains(bo.SubscribeCells, c => c.State == CellState.Failed);
    }

    [Fact]
    public async Task IsoCellSendsIsoForTheSlot()
    {
        var (vm, node, _) = Make(InMeeting);
        var ann = vm.Routing.Rows.Single(r => r.Name == "Ann");
        Assert.Equal(CellState.Armed, ann.IsoCells[0].State);
        // Every crosspoint stays drawn; only this guest's ISO column is live.
        Assert.All(ann.IsoCells.Skip(1), c => Assert.True(c.IsInteractive)); // a click routes Ann there
        await vm.Routing.ToggleAsync(ann.IsoCells[0]);
        Assert.Equal("""{"cmd":"iso","slot":1,"on":false}""", node.Sent.Single());
    }

    [Fact]
    public async Task UnroutingARecordingIsoIsRefusedLocallyWithTheReason()
    {
        var (vm, node, _) = Make(Recording);
        var ann = vm.Routing.Rows.Single(r => r.Name == "Ann");
        Assert.Equal(CellState.Recording, ann.IsoCells[0].State);
        await vm.Routing.ToggleAsync(ann.SubscribeCells[0]);
        Assert.Empty(node.Sent);
        Assert.Contains("Stop recording", ann.Detail);
    }

    [Fact]
    public void VideoOnFilterAndCapReadouts()
    {
        var (vm, _, _) = Make(InMeeting);
        Assert.Equal(new[] { "Ann", "Bo" }, vm.Routing.Rows.Select(r => r.Name));   // Cy has video off, self excluded
        Assert.Equal("Video on 2", vm.Routing.VideoOnLabel);
        Assert.Equal("All 3", vm.Routing.AllLabel);
        Assert.Equal("Subscribed 1/8", vm.Routing.SubscribedText);
        Assert.Equal("ISO 1/8", vm.Routing.IsoText);
        vm.Routing.VideoOnOnly = false;
        Assert.Equal(3, vm.Routing.Rows.Count);
    }

    [Fact]
    public void RosterCountsAreAbsentUntilTheRosterIsKnown()
    {
        var (vm, _, _) = Make("""{"type":"state","phase":"joining","maxSubscriptions":8}""");
        Assert.Equal("Video on", vm.Routing.VideoOnLabel);
        Assert.Equal("All", vm.Routing.AllLabel);
    }

    [Fact]
    public async Task RecordFormatSegmentsSendSetRecordFormat()
    {
        var (vm, node, _) = Make(InMeeting);
        await vm.Record.SetHeightCommand.ExecuteAsync(720);
        // Height only: the frame rate is per slot on Routing now.
        Assert.Equal("""{"cmd":"set-record-format","height":720}""", node.Sent[0]);
        Assert.True(vm.Record.Is720);                           // shown as pending selection
        Assert.True(vm.Record.FormatPending);
    }

    [Fact]
    public async Task FrameRateDefaultGoesWithNewRoutesAndEachSlotCanChangeIt()
    {
        var (vm, node, _) = Make(InMeeting);
        vm.Routing.DefaultFps = 60;
        var bo = vm.Routing.Rows.Single(r => r.Name == "Bo");
        await vm.Routing.ToggleAsync(bo.SubscribeCells[1]);
        Assert.Equal("""{"cmd":"subscribe","participantId":102,"on":true,"slot":2,"resolution":1080,"fps":60}""", node.Sent[0]);

        var ann = vm.Routing.Rows.Single(r => r.Name == "Ann");
        Assert.Equal(0, ann.Fps);                               // node default: as Zoom sends
        ann.Fps = 30;
        await vm.Routing.LastResolutionSend!;
        Assert.Equal("""{"cmd":"set-fps","slot":1,"fps":30}""", node.Sent[1]);
    }

    [Fact]
    public async Task ARoutedGuestsOtherCellsAreLiveAndMoveThem()
    {
        var (vm, node, _) = Make(InMeeting);
        var ann = vm.Routing.Rows.Single(r => r.Name == "Ann");
        Assert.All(ann.SubscribeCells.Skip(1), c => Assert.True(c.IsInteractive));
        await vm.Routing.ToggleAsync(ann.SubscribeCells[3]);
        // A move carries the guest's own settings: no resolution/fps sent.
        Assert.Equal("""{"cmd":"subscribe","participantId":101,"on":true,"slot":4}""", node.Sent.Single());
        Assert.Equal(CellState.Pending, ann.SubscribeCells[3].State);
    }

    [Fact]
    public async Task AnIsoCellOnAnotherColumnRoutesThere()
    {
        var (vm, node, _) = Make(InMeeting);
        var bo = vm.Routing.Rows.Single(r => r.Name == "Bo");
        Assert.True(bo.IsoCells[2].IsInteractive);
        await vm.Routing.ToggleAsync(bo.IsoCells[2]);
        Assert.Equal("""{"cmd":"subscribe","participantId":102,"on":true,"slot":3,"resolution":1080,"fps":0}""", node.Sent.Single());
    }

    [Fact]
    public async Task ClickingShowsNoPendingTextLine()
    {
        var (vm, _, _) = Make(InMeeting);
        var bo = vm.Routing.Rows.Single(r => r.Name == "Bo");
        vm.Routing.Rows.ToList();
        var task = vm.Routing.ToggleAsync(bo.SubscribeCells[1]);
        Assert.False(bo.HasDetail);                             // no line appears under the row
        await task;
    }

    [Fact]
    public async Task StartAndConfirmedStop()
    {
        var (vm, node, _) = Make(InMeeting);
        Assert.Null(vm.Record.StartBlockedReason);
        await vm.Record.PrimaryCommand.ExecuteAsync();
        Assert.Equal("""{"cmd":"record-start"}""", node.Sent.Single());

        vm.Apply(EdgeState.Parse(Recording));
        Assert.Equal("Stop recording", vm.Record.PrimaryText);
        await vm.Record.PrimaryCommand.ExecuteAsync();
        Assert.Single(node.Sent);                               // stop asks first
        Assert.True(vm.Record.ConfirmStopVisible);
        await vm.Record.ConfirmStopCommand.ExecuteAsync();
        Assert.Equal("""{"cmd":"record-stop"}""", node.Sent[1]);
    }

    [Fact]
    public void StartIsBlockedWithAReasonWhenTheEngineIsOff()
    {
        var (vm, _, _) = Make(InMeeting.Replace("\"engineOn\":true", "\"engineOn\":false"));
        Assert.Equal("Engine is off. Turn it on in Connect.", vm.Record.StartBlockedReason);
        Assert.False(vm.Record.PrimaryEnabled);
    }

    [Fact]
    public async Task FolderChoiceSendsSetRecordFolder()
    {
        var node = new RecordingNode();
        var shell = new FakeShell { Picked = @"E:\Takes" };
        var vm = new MainViewModel(node, null, shell, new FixedClock(TimeSpan.Zero));
        vm.Apply(EdgeState.Parse(InMeeting));
        string? saved = null;
        vm.Record.SaveSettings = (folder, _) => saved = folder;
        await vm.Record.ChangeFolderCommand.ExecuteAsync();
        Assert.Equal("""{"cmd":"set-record-folder","path":"E:\\Takes"}""", node.Sent.Single());
        Assert.Equal(@"E:\Takes", saved);
    }

    [Fact]
    public async Task FeedsNdiSendsImmediatelySrtWaitsForTheAddress()
    {
        var (vm, node, _) = Make(InMeeting);
        var feeds = vm.Routing.Feeds;
        Assert.True(feeds.Visible);
        await feeds.SelectNdiCommand.ExecuteAsync();
        Assert.Equal("""{"cmd":"set-feeds","mode":"ndi"}""", node.Sent.Single());

        await feeds.SelectSrtCommand.ExecuteAsync();
        Assert.Single(node.Sent);                               // SRT needs an address first
        Assert.True(feeds.IsSrt);
        feeds.Host = " 10.0.0.5 ";
        feeds.Port = "10000";
        feeds.Latency = "120";
        await feeds.ApplySrtCommand.ExecuteAsync();
        Assert.Equal("""{"cmd":"set-feeds","mode":"srt","srtHost":"10.0.0.5","srtBasePort":10000,"srtLatencyMs":120}""", node.Sent[1]);
    }

    [Fact]
    public async Task SrtPassphraseIsWriteOnly()
    {
        var (vm, node, _) = Make(InMeeting);
        var feeds = vm.Routing.Feeds;
        await feeds.SelectSrtCommand.ExecuteAsync();
        feeds.Host = "10.0.0.5";
        feeds.Passphrase = "s3cret-pass-phrase";
        await feeds.ApplySrtCommand.ExecuteAsync();
        Assert.Contains("\"srtPassphrase\":\"s3cret-pass-phrase\"", node.Sent.Single());
        Assert.Equal("", feeds.Passphrase);                     // dropped from the app once sent
    }

    [Fact]
    public async Task FeedRefusalIsShownVerbatim()
    {
        var (vm, node, _) = Make(InMeeting);
        node.Reply = _ => new EdgeCommandResult(false, "NDI runtime not found. Install NDI Tools.");
        await vm.Routing.Feeds.SelectNdiCommand.ExecuteAsync();
        Assert.Equal("NDI runtime not found. Install NDI Tools.", vm.Routing.Feeds.Error);
    }

    [Fact]
    public void TileFeedHealthComesFromTheNode()
    {
        var (vm, _, _) = Make(InMeeting.Replace("\"feeds\":{\"mode\":\"off\",\"slots\":[]}",
            "\"feeds\":{\"mode\":\"srt\",\"slots\":[{\"slot\":1,\"target\":\"x\",\"state\":\"failed\",\"framesSent\":0,\"framesSkipped\":0,\"connected\":false,\"error\":\"Pro is not listening at 10.0.0.5:10000 (turn on SRT input 1 in CoreVideo Pro). Retrying.\"}]}"));
        var tile = vm.Monitor.Tiles.Single();
        Assert.Equal("SRT \u00b7 failed", tile.FeedText);
        Assert.Equal(Tone.Error, tile.FeedTone);
        Assert.Equal("Pro is not listening at 10.0.0.5:10000 (turn on SRT input 1 in CoreVideo Pro). Retrying.", tile.FeedError);
    }

    [Fact]
    public void TileShowsNothingUnmeasured()
    {
        var (vm, _, _) = Make(InMeeting);
        var tile = vm.Monitor.Tiles.Single();
        Assert.Equal(1, tile.Slot);
        Assert.Equal("", tile.FpsText);                         // no fps measured: no "0 fps"
        Assert.Null(tile.AudioDb);                              // no audio measured: no meter
        Assert.False(tile.MeterVisible);
        Assert.False(tile.HasPicture);                          // no preview read: nothing drawn
    }

    [Fact]
    public void HeaderShowsOnlyWhatTheNodeReported()
    {
        var (vm, _, clock) = Make();
        Assert.False(vm.HeaderValuesVisible);
        Assert.False(vm.CapVisible);
        vm.Apply(EdgeState.Parse(Recording));
        Assert.True(vm.RecVisible);
        Assert.Equal("00:00:05", vm.RecTimer);
        Assert.Equal("1/8", vm.RoutedText);
        Assert.Equal(Tone.Rec, vm.MarkTone);

        clock.Now += TimeSpan.FromSeconds(3);
        vm.Tick();
        Assert.True(vm.IsStale);
        Assert.Equal("stale 3s", vm.StaleText);
        clock.Now += TimeSpan.FromSeconds(8);
        vm.Tick();
        Assert.True(vm.IsLost);                                 // > 10 s: values removed, reason shown
        Assert.False(vm.HeaderValuesVisible);
        Assert.False(vm.ScreensVisible);
        Assert.True(vm.BannerVisible);
    }

    [Fact]
    public void NodeExitIsLoud()
    {
        var (vm, _, _) = Make(InMeeting);
        vm.NodeExited("The Edge node exited (code 3).");
        Assert.True(vm.BannerVisible);
        Assert.Equal("The Edge node exited (code 3).", vm.Banner!.Problem);
    }

    [Fact]
    public void ScreenShortcutsSelectScreens()
    {
        var (vm, _, _) = Make();
        vm.SelectScreenCommand.Execute("Monitor");
        Assert.True(vm.IsMonitor);
        vm.SelectScreenCommand.Execute(Screen.Record);
        Assert.True(vm.IsRecord);
    }
}
