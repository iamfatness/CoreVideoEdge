using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;

namespace CoreVideoEdge.Desktop.Controls;

// The "Satellite Tile" mark (design brief section 6, direction B), drawn from
// the exact geometry of Assets/logo.svg on a 120 x 120 grid. Only the
// satellite fill carries state; the frame never changes colour.
public sealed class EdgeMark : Control
{
    public static readonly StyledProperty<IBrush?> StrokeProperty =
        AvaloniaProperty.Register<EdgeMark, IBrush?>(nameof(Stroke));
    public static readonly StyledProperty<IBrush?> SatelliteProperty =
        AvaloniaProperty.Register<EdgeMark, IBrush?>(nameof(Satellite));
    public static readonly StyledProperty<IBrush?> KnockoutProperty =
        AvaloniaProperty.Register<EdgeMark, IBrush?>(nameof(Knockout));

    // Frame: rect x=10 y=18 w=78 h=60 rx=13, stroke 6.
    internal static readonly Geometry Frame = Geometry.Parse(
        "M23,18 H75 A13,13 0 0 1 88,31 V65 A13,13 0 0 1 75,78 H23 A13,13 0 0 1 10,65 V31 A13,13 0 0 1 23,18 Z");
    // Cross: x=49 and y=48, stroke 5.
    internal static readonly Geometry Cross = Geometry.Parse("M49,18 V78 M10,48 H88");
    // Satellite tile: rect x=62 y=60 w=50 h=38 rx=8, fill, 6-unit knockout stroke underneath.
    internal static readonly Geometry Tile = Geometry.Parse(
        "M70,60 H104 A8,8 0 0 1 112,68 V90 A8,8 0 0 1 104,98 H70 A8,8 0 0 1 62,90 V68 A8,8 0 0 1 70,60 Z");

    static EdgeMark()
    {
        AffectsRender<EdgeMark>(StrokeProperty, SatelliteProperty, KnockoutProperty);
    }

    public IBrush? Stroke { get => GetValue(StrokeProperty); set => SetValue(StrokeProperty, value); }
    public IBrush? Satellite { get => GetValue(SatelliteProperty); set => SetValue(SatelliteProperty, value); }
    public IBrush? Knockout { get => GetValue(KnockoutProperty); set => SetValue(KnockoutProperty, value); }

    public override void Render(DrawingContext context) =>
        Draw(context, Bounds.Size, Stroke ?? Brushes.White, Satellite ?? Stroke ?? Brushes.White, Knockout ?? Brushes.Black);

    public static void Draw(DrawingContext context, Size size, IBrush stroke, IBrush satellite, IBrush knockout)
    {
        var scale = Math.Min(size.Width, size.Height) / 120.0;
        var dx = (size.Width - 120 * scale) / 2;
        var dy = (size.Height - 120 * scale) / 2;
        using (context.PushTransform(Matrix.CreateScale(scale, scale) * Matrix.CreateTranslation(dx, dy)))
        {
            context.DrawGeometry(null, new Pen(stroke, 6), Frame);
            context.DrawGeometry(null, new Pen(stroke, 5), Cross);
            // paint-order: stroke -> the knockout sits under the fill.
            context.DrawGeometry(null, new Pen(knockout, 6), Tile);
            context.DrawGeometry(satellite, null, Tile);
        }
    }
}
