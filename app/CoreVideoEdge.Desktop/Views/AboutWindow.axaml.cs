using System.Reflection;
using Avalonia.Controls;
using Avalonia.Media.Imaging;

namespace CoreVideoEdge.Desktop.Views;

// macOS "About CoreVideo Edge" (application menu). The icon is the same
// rendering the app icon is made from (Headless.RenderIcon).
public partial class AboutWindow : Window
{
    public AboutWindow()
    {
        InitializeComponent();
        VersionText.Text = $"Version {AppVersion}";
        AppIcon.Source = Headless.RenderIcon(144);
    }

    public static string AppVersion
    {
        get
        {
            var asm = typeof(AboutWindow).Assembly;
            var info = asm.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion;
            // Strip the "+<commit>" source-link suffix the SDK appends.
            return (info?.Split('+')[0]) ?? asm.GetName().Version?.ToString(3) ?? "unknown";
        }
    }
}
