using System.Runtime.InteropServices;
using Avalonia.Controls;
using Avalonia.Interactivity;
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

        // Elapsed counters, staleness and disk: 4 Hz.
        _tick = new DispatcherTimer(TimeSpan.FromMilliseconds(250), DispatcherPriority.Background, (_, _) => Vm?.Tick());
        // Preview frames: at most 15 fps per tile, only while the Monitor is shown.
        _preview = new DispatcherTimer(TimeSpan.FromMilliseconds(66), DispatcherPriority.Render, (_, _) =>
        {
            if (Vm is { IsMonitor: true } vm) vm.Monitor.PollPreviews();
        });
        Opened += (_, _) => { _tick.Start(); _preview.Start(); };
        SizeChanged += (_, e) => { if (Vm is { } vm) vm.CompactHeader = e.NewSize.Width < 1200; };
        DataContextChanged += (_, _) => { if (Vm is { } vm) vm.CompactHeader = Width < 1200; };

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

    // Windows: SPI_GETCLIENTAREAANIMATION. macOS (TODO when the Mac build lands):
    // NSWorkspace.accessibilityDisplayShouldReduceMotion.
    private static bool ReducedMotion()
    {
        if (Environment.GetEnvironmentVariable("COREVIDEO_EDGE_REDUCED_MOTION") == "1") return true;
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

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SystemParametersInfo(uint action, uint param, ref int value, uint winIni);
}
