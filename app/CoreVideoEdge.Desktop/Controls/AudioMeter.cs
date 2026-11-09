using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Layout;

namespace CoreVideoEdge.Desktop.Controls;

// Segmented level meter drawn ONLY from a measured dBFS value. Null = nothing
// measured = nothing drawn. Green to -18 dBFS, amber to -6, red above, with a
// 1.5 s peak hold. Vertical on tiles, horizontal in the ISO table.
public sealed class AudioMeter : Control
{
    public static readonly StyledProperty<double?> LevelDbProperty =
        AvaloniaProperty.Register<AudioMeter, double?>(nameof(LevelDb));
    public static readonly StyledProperty<Orientation> OrientationProperty =
        AvaloniaProperty.Register<AudioMeter, Orientation>(nameof(Orientation), Orientation.Vertical);

    private const double FloorDb = -60;
    private static readonly TimeSpan PeakHold = TimeSpan.FromSeconds(1.5);
    private double? _peak;
    private DateTime _peakAt;

    static AudioMeter()
    {
        AffectsRender<AudioMeter>(LevelDbProperty, OrientationProperty);
    }

    public double? LevelDb { get => GetValue(LevelDbProperty); set => SetValue(LevelDbProperty, value); }
    public Orientation Orientation { get => GetValue(OrientationProperty); set => SetValue(OrientationProperty, value); }

    protected override void OnPropertyChanged(AvaloniaPropertyChangedEventArgs change)
    {
        base.OnPropertyChanged(change);
        if (change.Property != LevelDbProperty) return;
        var now = DateTime.UtcNow;
        if (LevelDb is double db && (_peak is null || db >= _peak || now - _peakAt > PeakHold))
        {
            _peak = db;
            _peakAt = now;
        }
        if (LevelDb is null) _peak = null;
    }

    private static double Fraction(double db) => Math.Clamp((db - FloorDb) / -FloorDb, 0, 1);

    private IBrush Brush(string key) =>
        Application.Current!.TryGetResource(key, null, out var b) && b is IBrush brush ? brush : Brushes.Gray;

    public override void Render(DrawingContext context)
    {
        if (LevelDb is not double level) return;
        var size = Bounds.Size;
        var vertical = Orientation == Orientation.Vertical;
        var length = vertical ? size.Height : size.Width;
        var thick = vertical ? size.Width : size.Height;
        const double gap = 1;
        const int segments = 24;
        var seg = (length - gap * (segments - 1)) / segments;
        if (seg <= 0) return;
        var lit = Fraction(level) * segments;
        var green = Brush("LiveBrush");
        var amber = Brush("WarnBrush");
        var red = Brush("RecBrush");
        var off = Brush("SurfaceBrush");
        for (var i = 0; i < segments; ++i)
        {
            var db = FloorDb + (i + 1) * (-FloorDb / segments);
            var on = i < lit;
            var brush = !on ? off : db > -6 ? red : db > -18 ? amber : green;
            var start = i * (seg + gap);
            var rect = vertical
                ? new Rect(0, length - start - seg, thick, seg)
                : new Rect(start, 0, seg, thick);
            context.FillRectangle(brush, rect);
        }
        if (_peak is double peak && peak > FloorDb)
        {
            var i = Math.Clamp((int)(Fraction(peak) * segments) - 1, 0, segments - 1);
            var db = FloorDb + (i + 1) * (-FloorDb / segments);
            var start = i * (seg + gap);
            var rect = vertical ? new Rect(0, length - start - seg, thick, seg) : new Rect(start, 0, seg, thick);
            context.FillRectangle(db > -6 ? red : db > -18 ? amber : green, rect);
        }
    }
}
