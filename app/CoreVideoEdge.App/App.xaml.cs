using System.Windows;

namespace CoreVideoEdge.App;

public partial class App : Application
{
    private Mutex? _singleInstance;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        if (e.Args.Length == 3 && e.Args[0] == "--render-preview")
        {
            // Layout check without a visible window (never steals the desktop):
            // apply a captured node state and render the window to a PNG.
            var code = 0;
            try
            {
                new MainWindow(fake: true, launchCallback: null).RenderPreview(e.Args[1], e.Args[2]);
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine(ex);
                code = 1;
            }
            // Never linger: this mode must not leave a process (or window) behind.
            Environment.Exit(code);
        }
        var callback = OAuthCallbackRelay.FindCallbackArgument(e.Args);
        _singleInstance = new Mutex(true, $@"Local\CoreVideoEdge.App.{Environment.UserName}", out var first);
        if (!first)
        {
            // Started by the browser's corevideo:// redirect (or a second
            // launch): hand the callback to the running app and leave.
            if (callback is not null) OAuthCallbackRelay.TryForwardToPrimary(callback, TimeSpan.FromSeconds(3));
            Shutdown();
            return;
        }
        var fake = e.Args.Contains("--fake", StringComparer.OrdinalIgnoreCase) ||
                   Environment.GetEnvironmentVariable("COREVIDEO_EDGE_FAKE") == "1";
        var window = new MainWindow(fake, callback);
        MainWindow = window;
        window.Show();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        _singleInstance?.Dispose();
        base.OnExit(e);
    }
}
