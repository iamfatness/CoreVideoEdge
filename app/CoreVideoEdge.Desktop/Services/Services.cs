using CoreVideoPro.MediaCore.Services;

namespace CoreVideoEdge.Desktop.Services;

// What the view models need from the outside world. Real implementations talk
// to the node process, the lifted Zoom OAuth service and the OS; tests and the
// headless screenshot mode substitute their own.

public interface INodeChannel
{
    Task<EdgeCommandResult> SendAsync(string cmd, object? fields = null);
}

public sealed class EdgeNodeChannel(EdgeNodeClient client) : INodeChannel
{
    public Task<EdgeCommandResult> SendAsync(string cmd, object? fields = null) => client.SendAsync(cmd, fields);
}

public sealed record AccountStatus(bool SignedIn, bool Pending, string? Error = null);

public interface IAccountService
{
    Task<AccountStatus> GetStatusAsync();
    Task BeginSignInAsync();
    Task HandleCallbackAsync(string url);
    Task SignOutAsync();
    Task<(string? SdkJwt, string? UserZak)> GetJoinCredentialsAsync();
}

// The lifted CoreVideo Pro Zoom OAuth flow (broker https://corevideo.iamfatness.us)
// with the tokens protected by the platform seam (Windows: DPAPI).
public sealed class ZoomAccountService : IAccountService
{
    private readonly ZoomOAuthService _oauth;

    public ZoomAccountService(string tokenPath)
    {
        var protection = EdgeSecretProtection.ForCurrentPlatform();
        _oauth = new ZoomOAuthService(new FileZoomTokenStore(tokenPath, protection.Protect, protection.Unprotect));
    }

    public async Task<AccountStatus> GetStatusAsync()
    {
        var s = await _oauth.GetStatusAsync();
        return new AccountStatus(s.SignedIn, s.PendingAuthorization);
    }

    public async Task BeginSignInAsync()
    {
        var exe = Environment.ProcessPath ?? Path.Combine(AppContext.BaseDirectory, "CoreVideoEdge.exe");
        if (!OAuthCallbackRelay.TryRegisterProtocolHandler(exe, out var error))
            throw new InvalidOperationException($"Could not register the corevideo:// sign-in return: {error}");
        await _oauth.BeginAuthorizationAsync();
    }

    public Task HandleCallbackAsync(string url) => _oauth.HandleRedirectUrlAsync(url);

    public Task SignOutAsync() => _oauth.SignOutAsync();

    public async Task<(string? SdkJwt, string? UserZak)> GetJoinCredentialsAsync()
    {
        var c = await _oauth.EnsureJoinCredentialsAsync();
        return (c.SdkJwt, c.UserZak);
    }
}

public interface IShellServices
{
    Task<string?> PickFolderAsync(string? start);
    void OpenFolder(string path);
    // Free bytes on the volume holding `path`, or null when it cannot be measured.
    long? FreeBytes(string path);
    string? VolumeName(string path);
}

public sealed class OsShellServices : IShellServices
{
    public Func<string?, Task<string?>>? Picker { get; set; }

    public Task<string?> PickFolderAsync(string? start) => Picker?.Invoke(start) ?? Task.FromResult<string?>(null);

    public void OpenFolder(string path)
    {
        if (!Directory.Exists(path)) return;
        var opener = OperatingSystem.IsWindows() ? "explorer.exe" : OperatingSystem.IsMacOS() ? "open" : "xdg-open";
        System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(opener, $"\"{path}\"") { UseShellExecute = true });
    }

    public long? FreeBytes(string path)
    {
        try
        {
            var root = Path.GetPathRoot(Path.GetFullPath(path));
            return string.IsNullOrEmpty(root) ? null : new DriveInfo(root).AvailableFreeSpace;
        }
        catch (Exception ex) when (ex is IOException or ArgumentException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    public string? VolumeName(string path)
    {
        try
        {
            var root = Path.GetPathRoot(Path.GetFullPath(path));
            return string.IsNullOrEmpty(root) ? null : root.TrimEnd('\\', '/');
        }
        catch (ArgumentException)
        {
            return null;
        }
    }
}

public interface IClock
{
    TimeSpan Now { get; }
}

public sealed class SystemClock : IClock
{
    private readonly System.Diagnostics.Stopwatch _watch = System.Diagnostics.Stopwatch.StartNew();
    public TimeSpan Now => _watch.Elapsed;
}

public sealed class FixedClock(TimeSpan now) : IClock
{
    public TimeSpan Now { get; set; } = now;
}
