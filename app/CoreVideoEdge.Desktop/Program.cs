using Avalonia;

namespace CoreVideoEdge.Desktop;

internal sealed record LaunchOptions(bool Fake, string? Callback);

internal static class Program
{
    [STAThread]
    public static int Main(string[] args)
    {
        // Headless modes: never show a window, exit when done.
        if (args.Length > 0 && args[0] == "--screenshot") return Headless.Screenshot(args[1..]);
        if (args.Length > 0 && args[0] == "--render-brand") return Headless.RenderBrand(args[1..]);
        if (args.Length > 0 && args[0] == "--render-about") return Headless.RenderAbout(args[1..]);

        var callback = OAuthCallbackRelay.FindCallbackArgument(args);
        using var single = new Mutex(true, $@"Local\CoreVideoEdge.App.{Environment.UserName}", out var first);
        if (!first)
        {
            // Windows: started by the browser's corevideo:// redirect (or a second
            // launch): hand the callback to the running app and leave. macOS never
            // starts a second process for the URL (Launch Services sends an Apple
            // Event to the running app); a second copy just leaves.
            if (callback is not null && OperatingSystem.IsWindows())
                OAuthCallbackRelay.TryForwardToPrimary(callback, TimeSpan.FromSeconds(3));
            return 0;
        }
        var fake = args.Contains("--fake", StringComparer.OrdinalIgnoreCase) ||
                   Environment.GetEnvironmentVariable("COREVIDEO_EDGE_FAKE") == "1";
        App.Launch = new LaunchOptions(fake, callback);
        App.Callbacks.Deliver(callback);
        return BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
    }

    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<App>().UsePlatformDetect().LogToTrace();
}
