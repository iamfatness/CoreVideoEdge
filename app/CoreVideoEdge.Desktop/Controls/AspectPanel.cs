using Avalonia;
using Avalonia.Controls;

namespace CoreVideoEdge.Desktop.Controls;

// Lays its children out at a fixed aspect (16:9 by default) from the width it
// is given, so video areas letterbox instead of stretching.
public sealed class AspectPanel : Panel
{
    public static readonly StyledProperty<double> RatioProperty =
        AvaloniaProperty.Register<AspectPanel, double>(nameof(Ratio), 16.0 / 9.0);

    public double Ratio { get => GetValue(RatioProperty); set => SetValue(RatioProperty, value); }

    protected override Size MeasureOverride(Size availableSize)
    {
        var width = double.IsInfinity(availableSize.Width) ? 480 : availableSize.Width;
        var height = width / Ratio;
        if (!double.IsInfinity(availableSize.Height) && height > availableSize.Height)
        {
            height = availableSize.Height;
            width = height * Ratio;
        }
        var size = new Size(width, height);
        foreach (var child in Children) child.Measure(size);
        return size;
    }

    protected override Size ArrangeOverride(Size finalSize)
    {
        foreach (var child in Children) child.Arrange(new Rect(finalSize));
        return finalSize;
    }
}
