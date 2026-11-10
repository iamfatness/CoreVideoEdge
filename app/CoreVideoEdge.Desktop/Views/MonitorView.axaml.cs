using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.VisualTree;
using CoreVideoEdge.Desktop.ViewModels;

namespace CoreVideoEdge.Desktop.Views;

public partial class MonitorView : UserControl
{
    public MonitorView()
    {
        InitializeComponent();
        SizeChanged += (_, _) => FitTiles();
        TileGrid.LayoutUpdated += (_, _) => FitTiles();
    }

    // Tiles scale with the window (owner 2026-11-09): every column count is
    // tried and the one giving the largest picture that fits BOTH the width
    // and the height wins, so all routed guests are on screen without
    // scrolling. Below a floor width the grid scrolls instead of shrinking
    // further. Tiles keep their slot order, so a tile never jumps when another
    // route changes.
    internal const double TileMargin = 8;      // 4 px each side (tile Margin)
    internal const double GridMargin = 24;     // ItemsControl Margin 12 each side
    internal const double MinTileWidth = 240;
    internal const double DefaultStrip = 64;   // status strip under the picture until measured

    private void FitTiles()
    {
        var panel = TileGrid.GetVisualDescendants().OfType<WrapPanel>().FirstOrDefault();
        var count = TileGrid.ItemCount;
        if (panel is null || count == 0 || Bounds.Width <= 0 || Bounds.Height <= 0) return;
        var item = BestItemWidth(count, Bounds.Width - GridMargin, Bounds.Height - GridMargin, StripHeight(panel));
        if (Math.Abs(panel.ItemWidth - item) > 0.5) panel.ItemWidth = item;
    }

    // The strip under the picture, measured from a laid-out tile (it can wrap).
    private static double StripHeight(WrapPanel panel)
    {
        var tile = panel.Children.FirstOrDefault();
        if (tile is null || tile.Bounds.Width <= TileMargin) return DefaultStrip;
        var pictureWidth = tile.Bounds.Width - 2;                 // tile border
        var strip = tile.Bounds.Height - pictureWidth * 9 / 16 - 2;
        return strip > 0 && strip < 400 ? strip : DefaultStrip;
    }

    // Largest ItemWidth (picture 16:9 + strip, plus tile margin) for `count`
    // tiles in width x height. Pure: unit-tested.
    internal static double BestItemWidth(int count, double width, double height, double strip)
    {
        var best = MinTileWidth + TileMargin;
        for (var columns = 1; columns <= count; ++columns)
        {
            var rows = (int)Math.Ceiling(count / (double)columns);
            var byWidth = width / columns;
            var tileHeight = height / rows - TileMargin - strip;             // room for the picture
            var byHeight = tileHeight * 16 / 9 + TileMargin;
            var item = Math.Floor(Math.Min(byWidth, byHeight));
            if (item > best) best = item;
        }
        return Math.Min(best, Math.Floor(width));
    }

    private void OnScrimPressed(object? sender, PointerPressedEventArgs e)
    {
        if (DataContext is MonitorViewModel vm) vm.FocusedTile = null;
    }

    private void OnFlyoutPressed(object? sender, PointerPressedEventArgs e) => e.Handled = true;
}
