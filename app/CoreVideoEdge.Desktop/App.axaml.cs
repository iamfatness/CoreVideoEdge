using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using CoreVideoEdge.Desktop.Services;
using CoreVideoEdge.Desktop.ViewModels;
using CoreVideoEdge.Desktop.Views;

namespace CoreVideoEdge.Desktop;

public partial class App : Application
{
    // Set by Program before the lifetime starts (null in headless modes).
    internal static LaunchOptions? Launch { get; set; }

    public override void Initialize() => AvaloniaXamlLoader.Load(this);

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop && Launch is not null)
        {
            var window = BuildOperatorWindow(Launch, desktop);
            desktop.MainWindow = window;
        }
        base.OnFrameworkInitializationCompleted();
    }

    private static MainWindow BuildOperatorWindow(LaunchOptions launch, IClassicDesktopStyleApplicationLifetime desktop)
    {
        var configPath = EdgeSettings.DefaultConfigPath;
        EdgeSettings.EnsureExists(configPath);
        var logPath = Path.Combine(EdgeSettings.DataDirectory, "edge-node.log");
        var client = new EdgeNodeClient(logPath);
        var shell = new OsShellServices();
        var account = new ZoomAccountService(Path.Combine(EdgeSettings.DataDirectory, "zoom-oauth.json"));
        var options = new AppOptions
        {
            LogPath = logPath,
            JoinWithoutSignIn = launch.Fake || NodeHasJoinTokens(),
        };
        var vm = new MainViewModel(new EdgeNodeChannel(client), account, shell, new SystemClock(), options);
        vm.Record.SaveSettings = (folder, prefix) =>
        {
            try { EdgeSettings.Update(configPath, folder, prefix); }
            catch (Exception ex) { vm.ReportError(ex.Message); }
        };
        var window = new MainWindow { DataContext = vm };
        shell.Picker = async start =>
        {
            var startFolder = start is null ? null : await window.StorageProvider.TryGetFolderFromPathAsync(start);
            var picked = await window.StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions
            {
                Title = "Choose the ISO record folder",
                AllowMultiple = false,
                SuggestedStartLocation = startFolder,
            });
            return picked.Count > 0 ? picked[0].TryGetLocalPath() : null;
        };

        client.StateChanged += state => Dispatcher.UIThread.Post(() => vm.Apply(state));
        client.Exited += message => Dispatcher.UIThread.Post(() => { if (!window.ShuttingDown) vm.NodeExited(message); });
        var closing = new CancellationTokenSource();
        window.Opened += async (_, _) =>
        {
            _ = OAuthCallbackRelay.ServeAsync(url => Dispatcher.UIThread.Post(() => _ = vm.Connect.HandleCallbackAsync(url)), closing.Token);
            try
            {
                client.Start(ResolveNodeExecutable(), configPath, launch.Fake);
            }
            catch (Exception ex)
            {
                vm.NodeExited(ex.Message);
            }
            await vm.Connect.RefreshAccountAsync();
            if (launch.Callback is not null) await vm.Connect.HandleCallbackAsync(launch.Callback);
            // The node publishes on change only; ask once a second so a silent
            // node is noticed (stale after 2 s, values removed after 10 s).
            var poll = new DispatcherTimer(TimeSpan.FromSeconds(1), DispatcherPriority.Background, (_, _) =>
            {
                if (!window.ShuttingDown) _ = client.SendAsync("state");
            });
            poll.Start();
        };
        window.ShutdownNode = async () =>
        {
            closing.Cancel();
            await client.DisposeAsync();
        };
        return window;
    }

    private static bool NodeHasJoinTokens() =>
        new[] { "COREVIDEO_ZOOM_SDK_JWT", "COREVIDEO_ZOOM_USER_ZAK", "COREVIDEO_ZOOM_ON_BEHALF_TOKEN", "COREVIDEO_ZOOM_APP_PRIVILEGE_TOKEN" }
            .Any(name => !string.IsNullOrWhiteSpace(Environment.GetEnvironmentVariable(name)));

    // corevideo-edge(.exe) beside the app, then build/bin up the tree, or COREVIDEO_EDGE_NODE.
    internal static string ResolveNodeExecutable()
    {
        var name = OperatingSystem.IsWindows() ? "corevideo-edge.exe" : "corevideo-edge";
        var overridePath = Environment.GetEnvironmentVariable("COREVIDEO_EDGE_NODE");
        if (!string.IsNullOrWhiteSpace(overridePath)) return overridePath;
        var beside = Path.Combine(AppContext.BaseDirectory, name);
        if (File.Exists(beside)) return beside;
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
        {
            var candidate = Path.Combine(dir.FullName, "build", "bin", name);
            if (File.Exists(candidate)) return candidate;
        }
        return beside;
    }
}
