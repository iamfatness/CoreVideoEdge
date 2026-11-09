using System.Globalization;
using System.Xml.Linq;
using Xunit;

namespace CoreVideoEdge.Tests;

// Every text/background pair in the design brief section 3.2 is measured
// against the token file the app actually ships (Theme/Tokens.axaml), with the
// WCAG 2.x relative-luminance formula. A token edit that breaks a pair fails here.
public sealed class TokenContrastTests
{
    private static readonly Dictionary<string, (double R, double G, double B)> Tokens = Load();

    private static Dictionary<string, (double, double, double)> Load()
    {
        var path = Path.Combine(AppContext.BaseDirectory, "Fixtures", "Tokens.axaml");
        XNamespace x = "http://schemas.microsoft.com/winfx/2006/xaml";
        var doc = XDocument.Load(path);
        var result = new Dictionary<string, (double, double, double)>();
        foreach (var e in doc.Root!.Elements().Where(e => e.Name.LocalName == "Color"))
        {
            var key = (string)e.Attribute(x + "Key")!;
            var hex = e.Value.Trim().TrimStart('#');
            if (hex.Length == 8) hex = hex[2..];  // alpha tokens: only decorative hairlines, compared opaque
            result[key] = (
                int.Parse(hex[..2], NumberStyles.HexNumber) / 255.0,
                int.Parse(hex[2..4], NumberStyles.HexNumber) / 255.0,
                int.Parse(hex[4..6], NumberStyles.HexNumber) / 255.0);
        }
        return result;
    }

    private static double Linear(double c) => c <= 0.04045 ? c / 12.92 : Math.Pow((c + 0.055) / 1.055, 2.4);

    private static double Luminance(string token)
    {
        var (r, g, b) = Tokens[token + "Color"];
        return 0.2126 * Linear(r) + 0.7152 * Linear(g) + 0.0722 * Linear(b);
    }

    internal static double Ratio(string fg, string bg)
    {
        var a = Luminance(fg);
        var b = Luminance(bg);
        return (Math.Max(a, b) + 0.05) / (Math.Min(a, b) + 0.05);
    }

    private static readonly string[] Surfaces = ["Bg", "Panel", "Surface", "Raised", "Well", "Video"];

    public static IEnumerable<object[]> TextPairs() =>
        from fg in new[] { "Text", "Text2", "Accent", "Live", "Warn", "ErrorText" }
        from bg in Surfaces
        select new object[] { fg, bg, 4.5 };

    // Non-text UI (focus ring, control borders, the REC glyph): WCAG 1.4.11, 3:1.
    public static IEnumerable<object[]> UiPairs() =>
        (from fg in new[] { "Focus", "Rec" } from bg in Surfaces select new object[] { fg, bg, 3.0 })
        .Concat(from bg in Surfaces.Where(s => s != "Video") select new object[] { "CtlBorder", bg, 3.0 });

    public static IEnumerable<object[]> FillPairs() =>
    [
        ["OnAccent", "Accent", 4.5], ["OnAccent", "AccentHover", 4.5], ["OnAccent", "AccentPress", 4.5],
        ["OnLive", "Live", 4.5], ["OnWarn", "Warn", 4.5], ["OnRec", "Rec", 4.5],
        ["Text", "Hover", 4.5], ["Text2", "Hover", 4.5],
        ["Live", "LiveTint", 4.5], ["Warn", "WarnTint", 4.5], ["ErrorText", "ErrorTint", 4.5], ["Accent", "AccentTint", 4.5],
        ["Text", "ErrorTint", 4.5],
        ["Text2", "LiveTint", 4.5], ["Text2", "WarnTint", 4.5], ["Text2", "ErrorTint", 4.5], ["Text2", "AccentTint", 4.5],
        // Focus ring on the bg gap around a cyan matrix cell.
        ["Focus", "Bg", 3.0],
    ];

    [Theory]
    [MemberData(nameof(TextPairs))]
    [MemberData(nameof(UiPairs))]
    [MemberData(nameof(FillPairs))]
    public void PairMeetsItsRatio(string fg, string bg, double required)
    {
        var ratio = Ratio(fg, bg);
        Assert.True(ratio >= required, $"{fg} on {bg} is {ratio:0.00}:1, needs {required}:1");
    }

    [Theory]
    [InlineData("Text", "Bg", 16.72)]
    [InlineData("Text2", "Raised", 6.55)]
    [InlineData("CtlBorder", "Raised", 3.57)]
    [InlineData("Rec", "Raised", 4.11)]
    [InlineData("OnAccent", "Accent", 8.95)]
    [InlineData("OnRec", "Rec", 4.99)]
    [InlineData("ErrorText", "ErrorTint", 6.84)]
    public void RatiosMatchTheBrief(string fg, string bg, double brief) =>
        Assert.Equal(brief, Math.Round(Ratio(fg, bg), 2), 2);

    [Fact]
    public void RedTextIsNeverRecOnRaised()
    {
        // Why red text uses error-text: rec falls under 4.5 on raised.
        Assert.True(Ratio("Rec", "Raised") < 4.5);
        Assert.True(Ratio("ErrorText", "Raised") >= 4.5);
    }

    [Fact]
    public void DisabledTextIsOnlyForDisabled()
    {
        // text-disabled fails 4.5 by design (WCAG-exempt); this pins that it is never promoted to information.
        Assert.True(Ratio("TextDisabled", "Panel") < 4.5);
    }
}
