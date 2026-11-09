using System.Globalization;

namespace CoreVideoEdge.Desktop.ViewModels;

// Formatting of measured values. Nothing here invents a value: callers only
// format what the node (or the OS) measured.
public static class Format
{
    private static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

    public static string Clock(long ms)
    {
        var t = TimeSpan.FromMilliseconds(Math.Max(0, ms));
        return $"{(int)t.TotalHours:00}:{t.Minutes:00}:{t.Seconds:00}";
    }

    // Short elapsed counter for waits: 0:14, 1:05, 12:00.
    public static string Elapsed(TimeSpan t) => $"{(int)t.TotalMinutes}:{t.Seconds:00}";

    public static string Bytes(long bytes)
    {
        const double K = 1024;
        if (bytes >= K * K * K * K) return (bytes / (K * K * K * K)).ToString("0.0", Inv) + " TB";
        if (bytes >= K * K * K) return (bytes / (K * K * K)).ToString(bytes >= 100 * K * K * K ? "0" : "0.0", Inv) + " GB";
        if (bytes >= K * K) return (bytes / (K * K)).ToString("0", Inv) + " MB";
        if (bytes >= K) return (bytes / K).ToString("0", Inv) + " KB";
        return bytes.ToString(Inv) + " B";
    }

    public static string Size(int w, int h) => $"{w}×{h}";

    public static string Fps(double fps) => fps.ToString("0.0", Inv) + " fps";

    // 1080p59.9 style (slot headers). Height + measured rate.
    public static string Raster(int height, double? fps) =>
        fps is double f ? $"{height}p{f.ToString("0.#", Inv)}" : $"{height}p";

    public static string Count(long n) => n.ToString("N0", Inv);

    public static string Db(double db) => db.ToString("0.0", Inv) + " dBFS";

    public static string MeetingId(string id)
    {
        var digits = id.Trim();
        if (digits.Length == 11 && digits.All(char.IsDigit)) return $"{digits[..3]} {digits[3..7]} {digits[7..]}";
        if (digits.Length == 10 && digits.All(char.IsDigit)) return $"{digits[..3]} {digits[3..6]} {digits[6..]}";
        return digits;
    }

    // Middle-truncates a path so both the drive and the folder name stay visible.
    public static string MiddleTruncate(string path, int max)
    {
        if (path.Length <= max || max < 8) return path;
        var head = (max - 1) / 2;
        var tail = max - 1 - head;
        return path[..head] + "…" + path[^tail..];
    }

    public static string Plural(long n, string one, string many) => n == 1 ? $"{n} {one}" : $"{Count(n)} {many}";
}
