using CoreVideoEdge;
using Xunit;

namespace CoreVideoEdge.Tests;

// docs/PROTOCOL.md "New state fields (v2)".
public sealed class EdgeStateV2Tests
{
    [Fact]
    public void V2FieldsParse()
    {
        var state = EdgeState.Parse("""
            {"type":"state","seq":9,"phase":"in-meeting","maxSubscriptions":8,"engineOn":true,"movingRooms":true,
             "recordFormat":{"width":1280,"height":720,"fps":60},"previewDir":"C:\\Edge\\preview",
             "configPath":"C:\\Edge\\edge.json",
             "participants":[{"id":7,"name":"Ann","videoOn":true,"talking":true,"muted":false,"host":false,"slot":3,"iso":true,
                              "received":{"width":1920,"height":1080,"msSinceFrame":12,"receiving":true,"fps":59.94,"audioDb":-18.5}}],
             "subscriptions":[{"slot":3,"participantId":7,"name":"Ann","present":true,"iso":true,"fromConfig":false}],
             "recording":{"active":true,"startedUtc":"2026-11-09T10:00:00.000Z","elapsedMs":5000,
               "streams":[{"index":1,"slot":3,"name":"Ann","participantId":7,"path":"x.mp4","state":"recording",
                 "videoFrames":300,"videoLost":1,"videoDropped":2,"heldRepeats":3,"encoderBacklogPeak":4,
                 "cadenceRepeats":5,"cadenceSkipped":6,"audioSamples":240000,"silenceSamples":0,"audioLostPackets":0,
                 "msSinceVideo":10,"msSinceAudio":11,"bytes":123456}]}}
            """);

        Assert.True(state.EngineOn);
        Assert.True(state.MovingRooms);
        Assert.Equal(new EdgeRecordFormat(1280, 720, 60), state.RecordFormat);
        Assert.Equal(@"C:\Edge\preview", state.PreviewDir);
        Assert.Equal(@"C:\Edge\edge.json", state.ConfigPath);
        Assert.Equal("2026-11-09T10:00:00.000Z", state.RecordingStartedUtc);

        var ann = Assert.Single(state.Participants!);
        Assert.Equal(59.94, ann.ReceivedFps);
        Assert.Equal(-18.5, ann.AudioDb);
        Assert.Equal(12, ann.MsSinceFrame);

        var s = Assert.Single(state.Streams);
        Assert.Equal(7u, s.ParticipantId);
        Assert.Equal(2, s.VideoDropped);
        Assert.Equal(3, s.HeldRepeats);
        Assert.Equal(4, s.EncoderBacklogPeak);
        Assert.Equal(5, s.CadenceRepeats);
        Assert.Equal(6, s.CadenceSkipped);
        Assert.Equal(11, s.MsSinceAudio);
        Assert.Equal(123456, s.Bytes);
    }

    [Fact]
    public void V2FieldsAbsentStayAbsent()
    {
        var state = EdgeState.Parse("""
            {"type":"state","phase":"in-meeting","subscriptions":[],
             "participants":[{"id":7,"name":"Ann","videoOn":true,"slot":1,"iso":false,
                              "received":{"width":1280,"height":720,"msSinceFrame":400,"receiving":true}}],
             "recording":{"active":false,"streams":[{"index":1,"slot":1,"name":"Ann","path":"x","state":"recording",
               "videoFrames":1,"videoLost":0,"audioSamples":0,"silenceSamples":0,"audioLostPackets":0}]}}
            """);
        Assert.Null(state.EngineOn);            // not "off": the node did not say
        Assert.False(state.MovingRooms);
        Assert.Null(state.RecordFormat);
        Assert.Null(state.PreviewDir);
        var ann = Assert.Single(state.Participants!);
        Assert.Null(ann.ReceivedFps);           // not measured: no "0 fps"
        Assert.Null(ann.AudioDb);               // no audio measured: the meter is not drawn
        var s = Assert.Single(state.Streams);
        Assert.Null(s.CadenceRepeats);
        Assert.Null(s.Bytes);                   // size unknown until finalized
        Assert.Null(s.HeldRepeats);
    }

    [Fact]
    public void EngineOffIsFalseNotNull()
    {
        Assert.False(EdgeState.Parse("""{"type":"state","phase":"idle","engineOn":false}""").EngineOn);
    }

    [Fact]
    public void RecordFormatSourceRateIsZero()
    {
        var f = EdgeState.Parse("""{"type":"state","recordFormat":{"width":1920,"height":1080,"fps":0}}""").RecordFormat!;
        Assert.Equal(0, f.Fps);
        Assert.Equal(1080, f.Height);
    }

    [Fact]
    public void CapturedNodeStateLineParses()
    {
        // A line captured from corevideo-edge.exe --fake (docs/screenshots fixtures).
        var path = Path.Combine(AppContext.BaseDirectory, "Fixtures", "state-in-meeting.json");
        if (!File.Exists(path)) return;
        var state = EdgeState.Parse(File.ReadAllText(path));
        Assert.Equal("in-meeting", state.Phase);
        Assert.NotNull(state.Participants);
        Assert.True(state.MaxSubscriptions > 0);
    }
}
