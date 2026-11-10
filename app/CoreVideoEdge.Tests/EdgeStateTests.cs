using System.Text.Json;
using CoreVideoEdge;
using Xunit;

namespace CoreVideoEdge.Tests;

public sealed class EdgeStateTests
{
    [Fact]
    public void AbsentValuesStayAbsent()
    {
        using var doc = JsonDocument.Parse("""
            {"type":"state","seq":3,"phase":"in-meeting","maxSubscriptions":6,"recordFolder":"",
             "participants":[{"id":101,"name":"Alice","videoOn":true,"talking":false,"muted":true,"host":false,
                              "slot":1,"iso":true}],
             "subscriptions":[{"slot":1,"participantId":101,"name":"Alice","present":true,"iso":true,"fromConfig":false}],
             "recording":{"active":false,"streams":[]}}
            """);
        var state = EdgeState.Parse(doc.RootElement);

        Assert.True(state.InMeeting);
        Assert.Null(state.RawMediaActive);               // engine has not reported raw media yet
        var alice = Assert.Single(state.Participants!);
        Assert.Equal(1, alice.Slot);
        Assert.Null(alice.ReceivedWidth);                // nothing received yet: no size invented
        Assert.False(alice.Receiving);
    }

    [Fact]
    public void RosterUnknownIsNullNotEmpty()
    {
        using var doc = JsonDocument.Parse("""{"type":"state","phase":"joining","subscriptions":[],"recording":{"active":false,"streams":[]}}""");
        Assert.Null(EdgeState.Parse(doc.RootElement).Participants);
    }

    [Fact]
    public void RecordingHealthParses()
    {
        using var doc = JsonDocument.Parse("""
            {"type":"state","phase":"in-meeting","subscriptions":[],
             "recording":{"active":true,"elapsedMs":12000,"sessionDir":"D:\\ISO\\edge-20261108-220000",
               "streams":[{"index":1,"slot":2,"name":"Bob","path":"D:\\ISO\\x\\ISO-01-Bob.mp4","state":"recording",
                           "videoFrames":360,"videoLost":0,"audioSamples":576000,"silenceSamples":0,"audioLostPackets":0,
                           "encoder":"hardware","width":1920,"height":1080,"msSinceVideo":20}]},
             "rawMedia":{"active":true}}
            """);
        var state = EdgeState.Parse(doc.RootElement);
        Assert.True(state.Recording);
        Assert.Equal(12000, state.RecordingElapsedMs);
        Assert.True(state.RawMediaActive);
        var bob = Assert.Single(state.Streams);
        Assert.Equal("hardware", bob.Encoder);
        Assert.Equal(1920, bob.Width);
        Assert.Null(bob.Warning);
    }

    [Fact]
    public void CallbackUrlsAreRecognised()
    {
        Assert.True(OAuthCallbackRelay.IsOAuthCallbackUrl("corevideo://oauth/callback?state=x&broker_token=y"));
        Assert.False(OAuthCallbackRelay.IsOAuthCallbackUrl("https://corevideo.iamfatness.us/oauth/callback"));
        Assert.Equal("corevideo://oauth/callback?x", OAuthCallbackRelay.FindCallbackArgument(["--fake", "corevideo://oauth/callback?x"]));
    }

    [Fact]
    public void SettingsUpdatePreservesHandWrittenKeys()
    {
        var dir = Path.Combine(Path.GetTempPath(), "corevideo-edge-tests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        var path = Path.Combine(dir, "edge.json");
        File.WriteAllText(path, """{"subscribe":["Alice"],"maxSubscriptions":4}""");

        EdgeSettings.Update(path, recordFolder: @"D:\ISO");

        using var doc = JsonDocument.Parse(File.ReadAllText(path));
        Assert.Equal(@"D:\ISO", doc.RootElement.GetProperty("recordFolder").GetString());
        Assert.Equal(4, doc.RootElement.GetProperty("maxSubscriptions").GetInt32());
        Assert.Equal("Alice", doc.RootElement.GetProperty("subscribe")[0].GetString());
    }

    [Theory]
    [InlineData("""{"maxSubscriptions":6,"recordFolder":"D:\\Test"}""", 8)]                 // v1 default: lifted
    [InlineData("""{"maxSubscriptions":4}""", 4)]                                            // operator's value: kept
    [InlineData("""{"configVersion":2,"maxSubscriptions":6}""", 6)]                          // chosen after migration: kept
    public void V1DefaultCapOfSixIsMigratedOnce(string json, int expected)
    {
        var dir = Path.Combine(Path.GetTempPath(), "corevideo-edge-tests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        var path = Path.Combine(dir, "edge.json");
        File.WriteAllText(path, json);
        EdgeSettings.EnsureExists(path);
        using var doc = JsonDocument.Parse(File.ReadAllText(path));
        Assert.Equal(expected, doc.RootElement.GetProperty("maxSubscriptions").GetInt32());
        Assert.Equal(2, doc.RootElement.GetProperty("configVersion").GetInt32());
    }

    [Fact]
    public void ANewConfigStartsAtTheCapOfEight()
    {
        var path = Path.Combine(Path.GetTempPath(), "corevideo-edge-tests", Guid.NewGuid().ToString("N"), "edge.json");
        EdgeSettings.EnsureExists(path);
        using var doc = JsonDocument.Parse(File.ReadAllText(path));
        Assert.Equal(8, doc.RootElement.GetProperty("maxSubscriptions").GetInt32());
    }

    [Fact]
    public void SettingsUpdateRefusesToOverwriteBrokenJson()
    {
        var dir = Path.Combine(Path.GetTempPath(), "corevideo-edge-tests", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        var path = Path.Combine(dir, "edge.json");
        File.WriteAllText(path, "{ not json");
        Assert.Throws<InvalidOperationException>(() => EdgeSettings.Update(path, recordFolder: "x"));
        Assert.Equal("{ not json", File.ReadAllText(path));
    }
}
