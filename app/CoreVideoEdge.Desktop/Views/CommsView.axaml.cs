using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using CoreVideoEdge.Desktop.ViewModels;

namespace CoreVideoEdge.Desktop.Views;

// Panel-scoped keys (ZComms model): digits 1-9 key the first nine reachable
// cells, Space = ALL CALL, both momentary (or toggles in Latch mode). Never
// global: only while this screen has keyboard focus, never while typing in a
// field, and every held key goes up when the window loses focus.
public partial class CommsView : UserControl
{
    private Window? _window;
    private readonly HashSet<Key> _down = new();  // key auto-repeat must not re-toggle a latch

    public CommsView()
    {
        InitializeComponent();
        AddHandler(KeyDownEvent, OnKeyDownTunnel, Avalonia.Interactivity.RoutingStrategies.Tunnel);
        AddHandler(KeyUpEvent, OnKeyUpTunnel, Avalonia.Interactivity.RoutingStrategies.Tunnel);
        CellGrid.SizeChanged += (_, e) => FitColumns(e.NewSize.Width);
        CellGrid.LayoutUpdated += (_, _) => FitColumns(CellGrid.Bounds.Width);
        PropertyChanged += (_, e) =>
        {
            if (e.Property == IsVisibleProperty && IsVisible) Focus();
        };
    }

    private CommsViewModel? Vm => DataContext as CommsViewModel;

    // ZComms grid: auto-fill columns of at least MinCellWidth that share the
    // full width (CSS repeat(auto-fill, minmax(170px, 1fr))).
    internal const double MinCellWidth = 200;
    internal static int ColumnsFor(double width) => Math.Max(1, (int)Math.Floor(width / MinCellWidth));

    private void FitColumns(double width)
    {
        if (width <= 0 || CellGrid.ItemsPanelRoot is not Avalonia.Controls.Primitives.UniformGrid grid) return;
        var cols = ColumnsFor(width);
        if (grid.Columns != cols) grid.Columns = cols;
    }

    protected override void OnAttachedToVisualTree(VisualTreeAttachmentEventArgs e)
    {
        base.OnAttachedToVisualTree(e);
        _window = TopLevel.GetTopLevel(this) as Window;
        if (_window is not null) _window.Deactivated += OnDeactivated;
    }

    protected override void OnDetachedFromVisualTree(VisualTreeAttachmentEventArgs e)
    {
        if (_window is not null) _window.Deactivated -= OnDeactivated;
        _window = null;
        base.OnDetachedFromVisualTree(e);
    }

    private void OnDeactivated(object? sender, EventArgs e)
    {
        _down.Clear();
        _ = Vm?.ReleaseAllAsync();
    }

    private static bool Typing(KeyEventArgs e) => e.Source is TextBox or ComboBox or ComboBoxItem;

    private static int Digit(Key key) => key is >= Key.D1 and <= Key.D9 ? key - Key.D0
        : key is >= Key.NumPad1 and <= Key.NumPad9 ? key - Key.NumPad0 : 0;

    private void OnKeyDownTunnel(object? sender, KeyEventArgs e)
    {
        if (Vm is not { DeskVisible: true } vm || Typing(e) || e.KeyModifiers != KeyModifiers.None) return;
        if (e.Key == Key.Space || Digit(e.Key) > 0)
        {
            if (!_down.Add(e.Key)) { e.Handled = true; return; }
        }
        if (e.Key == Key.Space)
        {
            e.Handled = true;
            _ = vm.AllPressAsync();
            return;
        }
        if (Digit(e.Key) is var d and > 0 && vm.CellForDigit(d) is { } cell)
        {
            e.Handled = true;
            _ = vm.PressAsync(cell);
        }
    }

    private void OnKeyUpTunnel(object? sender, KeyEventArgs e)
    {
        _down.Remove(e.Key);
        if (Vm is not { } vm || Typing(e)) return;
        if (e.Key == Key.Space)
        {
            e.Handled = true;
            _ = vm.AllReleaseAsync();
            return;
        }
        if (Digit(e.Key) is var d and > 0 && vm.CellForDigit(d) is { } cell)
        {
            e.Handled = true;
            _ = vm.ReleaseAsync(cell);
        }
    }
}
