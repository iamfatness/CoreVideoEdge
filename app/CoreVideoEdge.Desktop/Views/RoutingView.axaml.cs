using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.VisualTree;
using CoreVideoEdge.Desktop.ViewModels;

namespace CoreVideoEdge.Desktop.Views;

public partial class RoutingView : UserControl
{
    // 240 name column + 8 x 80 subscribe + 16 gap + 8 x 36 ISO, plus margins.
    private const double BothBandsWidth = 240 + 8 * 80 + 16 + 8 * 36 + 32;
    private const double LeftColumnWidth = 320;

    public RoutingView()
    {
        InitializeComponent();
        Matrix.AddHandler(KeyDownEvent, OnMatrixKeyDown, handledEventsToo: false);
        SizeChanged += (_, e) => UpdateLayoutMode(e.NewSize.Width);
        DataContextChanged += (_, _) =>
        {
            if (Vm is null) return;
            Vm.Feeds.PropertyChanged += (_, e) =>
            {
                if (e.PropertyName == nameof(FeedsViewModel.Visible)) UpdateLayoutMode(Bounds.Width);
            };
        };
    }

    private RoutingViewModel? Vm => DataContext as RoutingViewModel;

    // The left list is optional at narrow widths; below the width both bands
    // need, the bands become Subscribe | ISO tabs over one grid.
    private void UpdateLayoutMode(double width)
    {
        if (width <= 0) return;
        // 240 name column + 8 x 80 subscribe cells: the narrowest matrix (one band).
        const double oneBand = 240 + 8 * 80 + 32;
        var showLeft = width >= LeftColumnWidth + oneBand;
        LeftColumn.IsVisible = showLeft;
        TopFeeds.IsVisible = !showLeft && Vm?.Feeds.Visible == true;
        if (Vm is { } vm) vm.Narrow = width - (showLeft ? LeftColumnWidth : 0) < BothBandsWidth;
    }

    // Arrow keys move through cells, Home/End jump in a row. Space/Enter toggle
    // (Button default). Hidden or disabled cells are skipped.
    private void OnMatrixKeyDown(object? sender, KeyEventArgs e)
    {
        if (e.Source is not Visual source) return;
        var button = source as Button ?? source.FindAncestorOfType<Button>();
        if (button?.DataContext is not CellViewModel cell || Vm is null) return;
        var rowIndex = Vm.Rows.IndexOf(cell.Row);
        if (rowIndex < 0) return;

        Button? target = e.Key switch
        {
            Key.Left => Neighbour(rowIndex, button, -1),
            Key.Right => Neighbour(rowIndex, button, +1),
            Key.Home => RowCells(rowIndex).FirstOrDefault(),
            Key.End => RowCells(rowIndex).LastOrDefault(),
            Key.Up => Nearest(rowIndex - 1, cell),
            Key.Down => Nearest(rowIndex + 1, cell),
            _ => null,
        };
        if (target is null) return;
        target.Focus(NavigationMethod.Directional);
        e.Handled = true;
    }

    private List<Button> RowCells(int rowIndex)
    {
        if (rowIndex < 0 || rowIndex >= Vm!.Rows.Count) return [];
        var container = Matrix.ContainerFromIndex(rowIndex);
        if (container is null) return [];
        return container.GetVisualDescendants().OfType<Button>()
            .Where(b => b.Classes.Contains("cell") && b.IsEffectivelyVisible && b.IsEffectivelyEnabled)
            .ToList();
    }

    private Button? Neighbour(int rowIndex, Button current, int step)
    {
        var cells = RowCells(rowIndex);
        var at = cells.IndexOf(current);
        var next = at + step;
        return at >= 0 && next >= 0 && next < cells.Count ? cells[next] : null;
    }

    private Button? Nearest(int rowIndex, CellViewModel from)
    {
        var cells = RowCells(rowIndex);
        return cells
            .Select(b => (Button: b, Cell: b.DataContext as CellViewModel))
            .Where(x => x.Cell is not null)
            .OrderBy(x => x.Cell!.Kind == from.Kind ? 0 : 1)
            .ThenBy(x => Math.Abs(x.Cell!.Column - from.Column))
            .Select(x => x.Button)
            .FirstOrDefault();
    }
}
