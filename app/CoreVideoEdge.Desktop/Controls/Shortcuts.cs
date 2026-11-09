using Avalonia.Input;

namespace CoreVideoEdge.Desktop.Controls;

// Shortcut text as each platform writes it: "Ctrl+1" on Windows, "⌘1" on macOS
// (Command replaces Control; Shift is ⇧). The key bindings themselves are
// declared once with Ctrl in XAML and moved to Command on macOS (ForPlatform).
public static class Shortcuts
{
    public static bool UseCommandKey { get; } = OperatingSystem.IsMacOS();

    public static string Connect => Text("Ctrl+1");
    public static string Routing => Text("Ctrl+2");
    public static string Monitor => Text("Ctrl+3");
    public static string Record => Text("Ctrl+4");
    public static string RecordToggle => Text("Ctrl+Shift+R");

    public static string Text(string windowsText, bool? mac = null)
    {
        if (!(mac ?? UseCommandKey)) return windowsText;
        var parts = windowsText.Split('+');
        var key = parts[^1];
        var mods = parts[..^1];
        // macOS order: Control, Option, Shift, Command.
        var result = "";
        if (mods.Contains("Alt")) result += "⌥";
        if (mods.Contains("Shift")) result += "⇧";
        if (mods.Contains("Ctrl")) result += "⌘";
        return result + key;
    }

    // The same gesture with Control moved to Command (Meta) on macOS.
    public static KeyGesture ForPlatform(KeyGesture gesture, bool? mac = null)
    {
        if (!(mac ?? UseCommandKey) || !gesture.KeyModifiers.HasFlag(KeyModifiers.Control)) return gesture;
        var mods = (gesture.KeyModifiers & ~KeyModifiers.Control) | KeyModifiers.Meta;
        return new KeyGesture(gesture.Key, mods);
    }
}
