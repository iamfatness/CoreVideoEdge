using CoreVideoEdge.Desktop.Views;
using Xunit;

namespace CoreVideoEdge.Tests;

// Monitor tiles scale with the window (owner 2026-11-09): the chosen tile
// width must fit every tile in BOTH dimensions and be as large as possible.
public sealed class MonitorFitTests
{
    private static (int Columns, int Rows, double Height) Layout(int count, double width, double item, double strip)
    {
        var columns = Math.Max(1, (int)Math.Floor(width / item));
        var rows = (int)Math.Ceiling(count / (double)columns);
        var tileHeight = (item - MonitorView.TileMargin) * 9 / 16 + strip + MonitorView.TileMargin;
        return (columns, rows, rows * tileHeight);
    }

    [Theory]
    [InlineData(8, 1256, 650, 64)]    // 1280x800 window, 8 guests
    [InlineData(8, 1896, 980, 64)]    // 1080p desktop
    [InlineData(6, 2536, 1340, 80)]   // 1440p, wrapped strip
    [InlineData(1, 1256, 650, 64)]    // one guest: limited by height, not width
    [InlineData(3, 900, 1400, 64)]    // tall window: stacks
    public void EveryTileFitsWithoutScrolling(int count, double width, double height, double strip)
    {
        var item = MonitorView.BestItemWidth(count, width, height, strip);
        var (_, _, used) = Layout(count, width, item, strip);
        Assert.True(used <= height + 0.5, $"{count} tiles at {item}px need {used}px of {height}px");
        Assert.True(item <= width);
    }

    [Fact]
    public void TilesGrowWithTheWindow()
    {
        var small = MonitorView.BestItemWidth(8, 1256, 650, 64);
        var large = MonitorView.BestItemWidth(8, 1896, 980, 64);
        Assert.True(large > small * 1.3, $"{small} -> {large}");
    }

    [Fact]
    public void OneGuestFillsTheHeightNotJustTheWidth()
    {
        // 650 px tall: picture height = 650 - 64 strip - 8 margin = 578 -> width ~1035, not 1256.
        var item = MonitorView.BestItemWidth(1, 1256, 650, 64);
        Assert.InRange(item, 1000, 1050);
    }

    [Fact]
    public void ATinyWindowScrollsRatherThanShrinkingPastTheFloor()
    {
        var item = MonitorView.BestItemWidth(8, 600, 200, 64);
        Assert.Equal(MonitorView.MinTileWidth + MonitorView.TileMargin, item);
    }
}
