using CoreVideoEdge;
using Xunit;

namespace CoreVideoEdge.Tests;

public sealed class PreviewReaderTests : IDisposable
{
    private readonly string _dir = Path.Combine(Path.GetTempPath(), "corevideo-edge-tests", "preview-" + Guid.NewGuid().ToString("N"));

    public PreviewReaderTests() => Directory.CreateDirectory(_dir);

    public void Dispose()
    {
        try { Directory.Delete(_dir, recursive: true); } catch (IOException) { }
    }

    private string SlotPath(int slot) => PreviewFileReader.SlotPath(_dir, slot);

    [Fact]
    public void NoFrameYetMeansNoPixelsAndNoMeasurements()
    {
        using var writer = new PreviewFileWriter(SlotPath(1));
        writer.Publish(2, 0, 0, 0, -1, int.MinValue, 42, heartbeat: 1000, lastFrame: 0, pixels: null);
        using var reader = PreviewFileReader.TryOpen(SlotPath(1), out var error)!;
        Assert.Null(error);

        var buffer = new byte[PreviewFileWriter.Stride * PreviewFileWriter.Height];
        var read = reader.TryRead(buffer);
        Assert.Equal(PreviewReadStatus.Ok, read.Status);
        Assert.False(read.PixelsCopied);           // frames == 0: nothing to show
        Assert.False(read.Header!.HasFrame);
        Assert.Null(read.Header.Fps);              // -1: not measured
        Assert.Null(read.Header.AudioDb);          // INT32_MIN: no audio yet
        Assert.Null(read.Header.FrameAgeMs);
        Assert.Equal(42u, read.Header.ParticipantId);
    }

    [Fact]
    public void FrameIsCopiedWithMeasuredValues()
    {
        using var writer = new PreviewFileWriter(SlotPath(2));
        var card = PreviewFileWriter.TestCard(255, 0, 0);
        writer.Publish(4, 1920, 1080, 300, 5994, -1850, 7, heartbeat: 20_000, lastFrame: 19_400, card);
        using var reader = PreviewFileReader.TryOpen(SlotPath(2), out _)!;

        var buffer = new byte[card.Length];
        var read = reader.TryRead(buffer);
        Assert.Equal(PreviewReadStatus.Ok, read.Status);
        Assert.True(read.PixelsCopied);
        Assert.Equal(card, buffer);
        var h = read.Header!;
        Assert.Equal(480, h.Width);
        Assert.Equal(270, h.Height);
        Assert.Equal(480 * 4, h.Stride);
        Assert.Equal(1920, h.SourceWidth);
        Assert.Equal(1080, h.SourceHeight);
        Assert.Equal(300u, h.Frames);
        Assert.Equal(59.94, h.Fps);
        Assert.Equal(-18.5, h.AudioDb);
        Assert.Equal(600, h.FrameAgeMs);           // heartbeat - lastFrame
    }

    [Fact]
    public void OddSeqIsNeverReadAsAFrame()
    {
        using var writer = new PreviewFileWriter(SlotPath(3));
        writer.Publish(2, 1920, 1080, 10, 3000, -2000, 1, 5000, 4990, PreviewFileWriter.TestCard(0, 255, 0));
        writer.SetSeq(3);                          // the node is mid-write and never finishes
        using var reader = PreviewFileReader.TryOpen(SlotPath(3), out _)!;
        var read = reader.TryRead(new byte[PreviewFileWriter.Stride * PreviewFileWriter.Height]);
        Assert.Equal(PreviewReadStatus.Busy, read.Status);
        Assert.Null(read.Header);
        Assert.False(read.PixelsCopied);
    }

    [Fact]
    public void ReaderSeesUpdatesThroughTheMapping()
    {
        using var writer = new PreviewFileWriter(SlotPath(4));
        writer.Publish(2, 1280, 720, 1, 2500, -3000, 1, 100, 100, PreviewFileWriter.TestCard(0, 0, 255));
        using var reader = PreviewFileReader.TryOpen(SlotPath(4), out _)!;
        Assert.Equal(1u, reader.TryRead(null).Header!.Frames);
        writer.Publish(4, 1280, 720, 2, 2500, -3000, 1, 200, 200, null);
        var h = reader.TryRead(null).Header!;
        Assert.Equal(2u, h.Frames);
        Assert.Equal(4u, h.Seq);
    }

    [Fact]
    public void BadMagicIsInvalid()
    {
        File.WriteAllBytes(SlotPath(5), new byte[PreviewFileReader.HeaderSize + 16]);
        using var reader = PreviewFileReader.TryOpen(SlotPath(5), out _)!;
        Assert.Equal(PreviewReadStatus.Invalid, reader.TryRead(null).Status);
    }

    [Fact]
    public void TruncatedFileIsRefused()
    {
        File.WriteAllBytes(SlotPath(6), new byte[10]);
        Assert.Null(PreviewFileReader.TryOpen(SlotPath(6), out var error));
        Assert.Contains("shorter", error);
    }

    [Fact]
    public void GeometryLargerThanFileIsInvalid()
    {
        using (var writer = new PreviewFileWriter(SlotPath(7)))
            writer.Publish(2, 0, 0, 0, -1, int.MinValue, 1, 1, 0, null);
        using (var fs = new FileStream(SlotPath(7), FileMode.Open)) fs.SetLength(PreviewFileReader.HeaderSize + 100);
        using var reader = PreviewFileReader.TryOpen(SlotPath(7), out _)!;
        Assert.Equal(PreviewReadStatus.Invalid, reader.TryRead(null).Status);
    }

    [Fact]
    public void MissingFileIsAnErrorNotACrash()
    {
        Assert.Null(PreviewFileReader.TryOpen(SlotPath(8), out var error));
        Assert.False(string.IsNullOrEmpty(error));
    }

    [Fact]
    public void NodeCanDeleteTheFileWhileItIsMapped()
    {
        var path = SlotPath(1);
        using var writer = new PreviewFileWriter(path);
        writer.Publish(2, 0, 0, 0, -1, int.MinValue, 1, 1, 0, null);
        using var reader = PreviewFileReader.TryOpen(path, out _)!;
        writer.Dispose();
        File.Delete(path);                          // the node unroutes the slot
        Assert.False(File.Exists(path));
    }

    [Fact]
    public void HeartbeatStallIsMeasuredOnTheAppClock()
    {
        var watch = new PreviewHeartbeatWatch(TimeSpan.FromSeconds(2));
        Assert.False(watch.IsStalled(TimeSpan.FromSeconds(100)));    // never observed: not "stalled"
        watch.Observe(500, TimeSpan.FromSeconds(0));
        watch.Observe(566, TimeSpan.FromSeconds(1));
        Assert.False(watch.IsStalled(TimeSpan.FromSeconds(2.5)));
        watch.Observe(566, TimeSpan.FromSeconds(2.5));               // same heartbeat: no change
        Assert.True(watch.IsStalled(TimeSpan.FromSeconds(3.1)));
        watch.Observe(632, TimeSpan.FromSeconds(3.2));
        Assert.False(watch.IsStalled(TimeSpan.FromSeconds(3.3)));
    }
}
