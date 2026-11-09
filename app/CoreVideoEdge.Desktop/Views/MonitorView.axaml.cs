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
        SizeChanged += (_, e) => FitColumns(e.NewSize.Width);
        TileGrid.LayoutUpdated += (_, _) => FitColumns(Bounds.Width);
    }

    // Auto-fit columns: 2 under 1000 px, 3 up to 1600 px, 4 above. Tiles keep
    // their slot order, so a tile never jumps when another route changes.
    private void FitColumns(double width)
    {
        if (width <= 0) return;
        var panel = TileGrid.GetVisualDescendants().OfType<WrapPanel>().FirstOrDefault();
        if (panel is null) return;
        var columns = width < 1000 ? 2 : width <= 1600 ? 3 : 4;
        var item = Math.Floor((width - 24) / columns);   // 12 px margins each side
        if (Math.Abs(panel.ItemWidth - item) > 0.5) panel.ItemWidth = item;
    }

    private void OnScrimPressed(object? sender, PointerPressedEventArgs e)
    {
        if (DataContext is MonitorViewModel vm) vm.FocusedTile = null;
    }

    private void OnFlyoutPressed(object? sender, PointerPressedEventArgs e) => e.Handled = true;
}
