using System.Runtime.InteropServices;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Platform;
using CoreVideoEdge.Desktop.Controls;
using Avalonia.Threading;
using CoreVideoEdge.Desktop.ViewModels;

namespace CoreVideoEdge.Desktop.Views;

public partial class MainWindow : Window
{
    private readonly DispatcherTimer _tick;
    private readonly DispatcherTimer _preview;
    private bool _closeConfirmed;

    public MainWindow()
    {
        InitializeComponent();
        if (ReducedMotion()) Classes.Add("reduced");
        if (OperatingSystem.IsMacOS()) ApplyMacChrome();

        // Elapsed counters, staleness and disk: 4 Hz.
        _tick = new DispatcherTimer(TimeSpan.FromMilliseconds(250), DispatcherPriority.Background, (_, _) => Vm?.Tick());
        // Preview frames: at most 15 fps per tile, only while the Monitor is shown.
        _preview = new DispatcherTimer(TimeSpan.FromMilliseconds(66), DispatcherPriority.Render, (_, _) =>
        {
            if (Vm is { IsMonitor: true } vm) vm.Monitor.PollPreviews();
        });
        Opened += (_, _) => { _tick.Start(); _preview.Start(); };
        SizeChanged += (_, e) => { if (Vm is { } vm) vm.CompactHeader = e.NewSize.Width < CompactBelow; };
        DataContextChanged += (_, _) => { if (Vm is { } vm) vm.CompactHeader = Width < CompactBelow; };

        KeepRecordingButton.Click += (_, _) => CloseConfirm.IsVisible = false;
        StopAndCloseButton.Click += (_, _) =>
        {
            CloseConfirm.IsVisible = false;
            _closeConfirmed = true;
            Close();
        };
    }

    private MainViewModel? Vm => DataContext as MainViewModel;

    // Set by the app: stops the node (finalizes files) before the window goes.
    public Func<Task>? ShutdownNode { get; set; }
    public bool ShuttingDown { get; private set; }

    protected override async void OnClosing(WindowClosingEventArgs e)
    {
        base.OnClosing(e);
        if (ShuttingDown && ShutdownNode is null) return;
        if (Vm?.State?.Recording == true && !_closeConfirmed)
        {
            e.Cancel = true;
            CloseConfirm.IsVisible = true;
            KeepRecordingButton.Focus();
            return;
        }
        if (ShutdownNode is { } shutdown)
        {
            e.Cancel = true;
            ShutdownNode = null;
            ShuttingDown = true;
            IsEnabled = false;
            _tick.Stop();
            _preview.Stop();
            await shutdown();
            Close();
        }
    }

    // Left inset that clears the macOS traffic lights (12 + 3 x 14 + 2 x 8 gaps,
    // plus room before the mark); 16 when they are hidden (full screen).
    internal const double MacTrafficLightInset = 80;

    // Below this width the wordmark gives way to the mark (MainViewModel.CompactHeader).
    // macOS spends MacTrafficLightInset on the traffic lights, so the full
    // header (with the REC tally) needs that much more room there.
    internal static double CompactBelow => OperatingSystem.IsMacOS() ? 1360 : 1200;

    // macOS: content extends under a transparent title bar, the 48 px header is
    // the title bar (traffic lights at its left), Ctrl shortcuts become Command.
    // Windows keeps its system title bar and Ctrl shortcuts untouched.
    private void ApplyMacChrome()
    {
        ExtendClientAreaToDecorationsHint = true;
        ExtendClientAreaChromeHints = ExtendClientAreaChromeHints.PreferSystemChrome;
        ExtendClientAreaTitleBarHeightHint = 48;
        HeaderContent.Margin = new Thickness(MacTrafficLightInset, 0, 16, 0);
        PropertyChanged += (_, e) =>
        {
            if (e.Property == WindowStateProperty)
                HeaderContent.Margin = new Thickness(WindowState == WindowState.FullScreen ? 16 : MacTrafficLightInset, 0, 16, 0);
        };
        // Drag the window by the header's empty space; double-click zooms, as a
        // title bar does. Buttons and pills keep their own clicks.
        HeaderBar.PointerPressed += (_, e) =>
        {
            if (!e.GetCurrentPoint(this).Properties.IsLeftButtonPressed || IsInsideButton(e.Source)) return;
            if (e.ClickCount == 2)
                WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;
            else
                BeginMoveDrag(e);
        };
        foreach (var binding in KeyBindings)
            if (binding.Gesture is { } gesture) binding.Gesture = Shortcuts.ForPlatform(gesture);
    }

    private static bool IsInsideButton(object? source)
    {
        for (var v = source as StyledElement; v is not null; v = v.Parent)
            if (v is Button) return true;
        return false;
    }

    // Windows: SPI_GETCLIENTAREAANIMATION. macOS: NSWorkspace
    // accessibilityDisplayShouldReduceMotion (System Settings > Accessibility >
    // Display > Reduce motion).
    private static bool ReducedMotion()
    {
        if (Environment.GetEnvironmentVariable("COREVIDEO_EDGE_REDUCED_MOTION") == "1") return true;
        if (OperatingSystem.IsMacOS()) return MacReducedMotion();
        if (!OperatingSystem.IsWindows()) return false;
        try
        {
            var enabled = 1;
            return SystemParametersInfo(0x1042, 0, ref enabled, 0) && enabled == 0;
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException)
        {
            return false;
        }
    }

    private static bool MacReducedMotion()
    {
        try
        {
            var workspace = objc_msgSend(objc_getClass("NSWorkspace"), sel_registerName("sharedWorkspace"));
            return workspace != IntPtr.Zero &&
                   objc_msgSend_bool(workspace, sel_registerName("accessibilityDisplayShouldReduceMotion"));
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException)
        {
            return false;
        }
    }

    private const string ObjC = "/usr/lib/libobjc.A.dylib";
    [DllImport(ObjC)] private static extern IntPtr objc_getClass(string name);
    [DllImport(ObjC)] private static extern IntPtr sel_registerName(string name);
    [DllImport(ObjC, EntryPoint = "objc_msgSend")] private static extern IntPtr objc_msgSend(IntPtr receiver, IntPtr selector);
    [DllImport(ObjC, EntryPoint = "objc_msgSend")]
    [return: MarshalAs(UnmanagedType.I1)]
    private static extern bool objc_msgSend_bool(IntPtr receiver, IntPtr selector);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SystemParametersInfo(uint action, uint param, ref int value, uint winIni);
}
