using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;

namespace CoreVideoEdge.Desktop.Controls;

// A Lucide icon: a 24-unit stroke geometry drawn at the control's size with a
// 1.75-unit round stroke (brief section 3.7).
public sealed class Icon : Control
{
    public static readonly StyledProperty<Geometry?> DataProperty =
        AvaloniaProperty.Register<Icon, Geometry?>(nameof(Data));
    public static readonly StyledProperty<IBrush?> ForegroundProperty =
        AvaloniaProperty.Register<Icon, IBrush?>(nameof(Foreground));

    static Icon()
    {
        AffectsRender<Icon>(DataProperty, ForegroundProperty);
        WidthProperty.OverrideDefaultValue<Icon>(16);
        HeightProperty.OverrideDefaultValue<Icon>(16);
    }

    public Geometry? Data { get => GetValue(DataProperty); set => SetValue(DataProperty, value); }
    public IBrush? Foreground { get => GetValue(ForegroundProperty); set => SetValue(ForegroundProperty, value); }

    public override void Render(DrawingContext context)
    {
        if (Data is null || Foreground is null) return;
        var scale = Math.Min(Bounds.Width, Bounds.Height) / 24.0;
        using (context.PushTransform(Matrix.CreateScale(scale, scale)))
        {
            context.DrawGeometry(null, new Pen(Foreground, 1.75, lineCap: PenLineCap.Round, lineJoin: PenLineJoin.Round), Data);
        }
    }
}
