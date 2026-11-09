using System.Globalization;
using Avalonia;
using Avalonia.Data.Converters;
using Avalonia.Media;
using CoreVideoEdge.Desktop.ViewModels;

namespace CoreVideoEdge.Desktop.Controls;

// Tone -> token brush. Parameter: "fg" (text/icon on dark), "tint" (fill
// behind a tone), "fill" (solid tally fill), "ink" (text on that fill).
public sealed class ToneBrushConverter : IValueConverter
{
    public static readonly ToneBrushConverter Instance = new();

    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        var tone = value is Tone t ? t : Tone.Neutral;
        var key = (parameter as string ?? "fg") switch
        {
            "tint" => tone switch
            {
                Tone.Live => "LiveTintBrush",
                Tone.Warn => "WarnTintBrush",
                Tone.Error or Tone.Rec => "ErrorTintBrush",
                Tone.Accent => "AccentTintBrush",
                _ => "SurfaceBrush",
            },
            "fill" => tone switch
            {
                Tone.Live => "LiveBrush",
                Tone.Warn => "WarnBrush",
                Tone.Error => "ErrorTextBrush",
                Tone.Rec => "RecBrush",
                Tone.Accent => "AccentBrush",
                _ => "CtlBorderBrush",
            },
            "ink" => tone switch
            {
                Tone.Live => "OnLiveBrush",
                Tone.Warn => "OnWarnBrush",
                Tone.Rec or Tone.Error => "OnRecBrush",
                Tone.Accent => "OnAccentBrush",
                _ => "TextBrush",
            },
            "border" => tone switch
            {
                Tone.Live => "LiveBrush",
                Tone.Warn => "WarnBrush",
                Tone.Error => "ErrorTextBrush",
                Tone.Rec => "RecBrush",
                Tone.Accent => "AccentBrush",
                _ => "HairlineBrush",
            },
            _ => tone switch
            {
                Tone.Live => "LiveBrush",
                Tone.Warn => "WarnBrush",
                Tone.Error or Tone.Rec => "ErrorTextBrush",  // red text is always error-text, never rec
                Tone.Accent => "AccentBrush",
                _ => "Text2Brush",
            },
        };
        return Application.Current!.TryGetResource(key, null, out var brush) ? brush : Brushes.Magenta;
    }

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture) =>
        throw new NotSupportedException();
}

// Tone -> icon geometry: warn and error always carry an icon (colour is never
// the only signal).
public sealed class ToneIconConverter : IValueConverter
{
    public static readonly ToneIconConverter Instance = new();

    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        var key = value switch
        {
            Tone.Warn => "IconAlertTriangle",
            Tone.Error => "IconAlertCircle",
            Tone.Live => "IconCheckCircle",
            _ => "IconCircle",
        };
        return Application.Current!.TryGetResource(key, null, out var g) ? g : null;
    }

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture) =>
        throw new NotSupportedException();
}

public sealed class ToneIsConverter : IValueConverter
{
    public static readonly ToneIsConverter Instance = new();

    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture) =>
        value is Tone t && parameter is string p && p.Split(',').Any(x => Enum.TryParse<Tone>(x, out var want) && want == t);

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture) =>
        throw new NotSupportedException();
}
