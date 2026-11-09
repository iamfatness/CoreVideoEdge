using System.Globalization;
using Avalonia;
using Avalonia.Headless;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Threading;
using CoreVideoEdge.Desktop.Controls;
using CoreVideoEdge.Desktop.Services;
using CoreVideoEdge.Desktop.ViewModels;
using CoreVideoEdge.Desktop.Views;

namespace CoreVideoEdge.Desktop;

// Modes that render without a visible window (never touch the desktop) and exit.
internal static class Headless
{
    private const string Usage =
        "usage: CoreVideoEdge --screenshot <state.json> <connect|routing|monitor|record> <out.png> " +
        "[--preview-dir <dir>] [--signed-in] [--sign-in-pending] [--meeting-link <text>] [--size WxH] [--state-age <s>]";

    // --screenshot: apply a CAPTURED node state line and render one screen.
    public static int Screenshot(string[] args)
    {
        if (args.Length < 3) { Console.Error.WriteLine(Usage); return 2; }
        var statePath = args[0];
        if (!Enum.TryParse<Screen>(args[1], ignoreCase: true, out var screen)) { Console.Error.WriteLine(Usage); return 2; }
        var outPath = args[2];
        string? previewDir = null, meetingLink = null;
        bool signedIn = false, pending = false;
        int width = 1280, height = 800;
        double stateAge = 0;
        for (var i = 3; i < args.Length; ++i)
        {
            switch (args[i])
            {
                case "--preview-dir": previewDir = args[++i]; break;
                case "--signed-in": signedIn = true; break;
                case "--sign-in-pending": pending = true; break;
                case "--meeting-link": meetingLink = args[++i]; break;
                case "--state-age": stateAge = double.Parse(args[++i], CultureInfo.InvariantCulture); break;
                case "--size":
                    var parts = args[++i].Split('x');
                    width = int.Parse(parts[0], CultureInfo.InvariantCulture);
                    height = int.Parse(parts[1], CultureInfo.InvariantCulture);
                    break;
                default: Console.Error.WriteLine($"unknown option {args[i]}\n{Usage}"); return 2;
            }
        }

        try
        {
            Setup();
            var state = EdgeState.Parse(File.ReadAllText(statePath));
            var clock = new FixedClock(TimeSpan.FromSeconds(1000));
            var vm = new MainViewModel(new HeadlessNode(), new PreviewAccount(signedIn, pending), new OsShellServices(), clock,
                new AppOptions { JoinWithoutSignIn = state.EngineKind == "fake" });
            vm.Connect.SetAccountForPreview(signedIn, pending);
            if (meetingLink is not null) vm.Connect.MeetingLink = meetingLink;
            vm.Monitor.PreviewDirOverride = previewDir;
            vm.Apply(state);
            vm.SelectedScreen = screen;
            var window = new MainWindow { DataContext = vm, Width = width, Height = height };
            window.Show();
            for (var i = 0; i < 4; ++i)
            {
                vm.Monitor.PollPreviews();
                vm.Tick();
                Pump();
            }
            if (stateAge > 0)
            {
                clock.Now += TimeSpan.FromSeconds(stateAge);
                vm.Tick();
                Pump();
            }
            var frame = window.CaptureRenderedFrame() ?? throw new InvalidOperationException("Nothing was rendered.");
            Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(outPath))!);
            frame.Save(outPath);
            window.Close();
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex);
            return 1;
        }
    }

    // --render-brand <dir>: the app icon (Pro's squircle recipe + the Edge mark)
    // at 16/24/32/48/64/256 packed into CoreVideoEdge.ico, plus a 1024 PNG for macOS.
    public static int RenderBrand(string[] args)
    {
        if (args.Length < 1) { Console.Error.WriteLine("usage: CoreVideoEdge --render-brand <outDir>"); return 2; }
        try
        {
            Setup();
            var dir = Directory.CreateDirectory(args[0]).FullName;
            var icoSizes = new[] { 16, 24, 32, 48, 64, 256 };
            var pngs = new List<(int Size, byte[] Png)>();
            foreach (var size in icoSizes.Append(1024))
            {
                using var bitmap = RenderIcon(size);
                using var stream = new MemoryStream();
                bitmap.Save(stream);
                if (size == 1024) File.WriteAllBytes(Path.Combine(dir, "icon-1024.png"), stream.ToArray());
                else pngs.Add((size, stream.ToArray()));
            }
            File.WriteAllBytes(Path.Combine(dir, "CoreVideoEdge.ico"), BuildIco(pngs));
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex);
            return 1;
        }
    }

    internal static RenderTargetBitmap RenderIcon(int size)
    {
        var bitmap = new RenderTargetBitmap(new PixelSize(size, size));
        using (var ctx = bitmap.CreateDrawingContext())
        {
            // Squircle: 36 x 36 at r 9 -> 120-unit grid at rx 30; gradient #15191B -> #0B0D0E, (0,0) -> (0.35,1).
            var s = size / 120.0;
            var gradient = new LinearGradientBrush
            {
                StartPoint = new RelativePoint(0, 0, RelativeUnit.Relative),
                EndPoint = new RelativePoint(0.35, 1, RelativeUnit.Relative),
                GradientStops = { new GradientStop(Color.Parse("#15191B"), 0), new GradientStop(Color.Parse("#0B0D0E"), 1) },
            };
            ctx.DrawRectangle(gradient, null, new RoundedRect(new Rect(0, 0, size, size), 30 * s));
            // Padding 6 of 36 -> 20 units each side.
            using (ctx.PushTransform(Matrix.CreateTranslation(20 * s, 20 * s)))
            {
                var accent = new SolidColorBrush(Color.Parse("#3CC4E0"));
                EdgeMark.Draw(ctx, new Size(80 * s, 80 * s), accent, accent, new SolidColorBrush(Color.Parse("#0B0D0E")));
            }
        }
        return bitmap;
    }

    // ICO with PNG-compressed entries (Windows Vista and later).
    internal static byte[] BuildIco(IReadOnlyList<(int Size, byte[] Png)> images)
    {
        using var ms = new MemoryStream();
        using var w = new BinaryWriter(ms);
        w.Write((ushort)0);
        w.Write((ushort)1);
        w.Write((ushort)images.Count);
        var offset = 6 + 16 * images.Count;
        foreach (var (size, png) in images)
        {
            w.Write((byte)(size >= 256 ? 0 : size));
            w.Write((byte)(size >= 256 ? 0 : size));
            w.Write((byte)0);
            w.Write((byte)0);
            w.Write((ushort)1);
            w.Write((ushort)32);
            w.Write(png.Length);
            w.Write(offset);
            offset += png.Length;
        }
        foreach (var (_, png) in images) w.Write(png);
        return ms.ToArray();
    }

    private static void Setup() =>
        AppBuilder.Configure<App>()
            .UseSkia()
            .UseHeadless(new AvaloniaHeadlessPlatformOptions { UseHeadlessDrawing = false })
            .SetupWithoutStarting();

    private static void Pump()
    {
        for (var i = 0; i < 3; ++i)
        {
            Dispatcher.UIThread.RunJobs();
            AvaloniaHeadlessPlatform.ForceRenderTimerTick();
        }
    }

    // Screenshots have no token store: the account state is a command-line input
    // (--signed-in / --sign-in-pending) and every account action is refused.
    private sealed class PreviewAccount(bool signedIn, bool pending) : IAccountService
    {
        private static Exception No() => new InvalidOperationException("Screenshot mode: no Zoom account.");
        public Task<AccountStatus> GetStatusAsync() => Task.FromResult(new AccountStatus(signedIn, pending));
        public Task BeginSignInAsync() => throw No();
        public Task HandleCallbackAsync(string url) => throw No();
        public Task SignOutAsync() => throw No();
        public Task<(string? SdkJwt, string? UserZak)> GetJoinCredentialsAsync() => throw No();
    }

    // Screenshots never talk to a node: every action is answered "not running".
    private sealed class HeadlessNode : INodeChannel
    {
        public Task<EdgeCommandResult> SendAsync(string cmd, object? fields = null) =>
            Task.FromResult(new EdgeCommandResult(false, "Screenshot mode: no node."));
    }
}
