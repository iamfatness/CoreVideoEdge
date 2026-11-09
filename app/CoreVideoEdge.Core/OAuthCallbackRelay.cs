using System.IO.Pipes;
using System.Text;

namespace CoreVideoEdge;

// Gets the browser's corevideo://oauth/callback?... back into the running app.
//
// The deployed broker (https://corevideo.iamfatness.us) only returns to
// corevideo://oauth/callback (CoreVideo Pro docs/reference/secrets-and-oauth.md),
// so Edge registers that scheme for the current user. Windows starts a SECOND
// app process with the URL as its argument; that process hands the URL to the
// first one over a per-user named pipe and exits. The OAuth service then checks
// the callback's state against the sign-in it started, so a forged callback is
// rejected there.
public static class OAuthCallbackRelay
{
    public const string Protocol = "corevideo";

    private static string PipeName => $"CoreVideoEdge.OAuthCallback.{Environment.UserName}";

    public static bool IsOAuthCallbackUrl(string? value) =>
        !string.IsNullOrWhiteSpace(value) &&
        value.StartsWith($"{Protocol}://", StringComparison.OrdinalIgnoreCase);

    public static string? FindCallbackArgument(IEnumerable<string> args) => args.FirstOrDefault(IsOAuthCallbackUrl);

    // Points corevideo:// at this executable for the current user (HKCU), the
    // same registration CoreVideo Pro's unpackaged shell writes. Called when the
    // operator clicks Sign in, not at startup: on a machine that also runs
    // CoreVideo Pro, the most recent sign-in owns the scheme.
    public static bool TryRegisterProtocolHandler(string exePath, out string? error)
    {
        error = null;
        try
        {
            using var protocolKey = Microsoft.Win32.Registry.CurrentUser.CreateSubKey($@"Software\Classes\{Protocol}");
            protocolKey.SetValue("", $"URL:{Protocol} Protocol");
            protocolKey.SetValue("URL Protocol", "");
            using var commandKey = protocolKey.CreateSubKey(@"shell\open\command");
            commandKey.SetValue("", $"\"{exePath}\" \"%1\"");
            return true;
        }
        catch (Exception ex)
        {
            error = ex.Message;
            return false;
        }
    }

    public static bool TryForwardToPrimary(string callbackUrl, TimeSpan timeout)
    {
        try
        {
            using var client = new NamedPipeClientStream(".", PipeName, PipeDirection.Out);
            client.Connect((int)timeout.TotalMilliseconds);
            var bytes = Encoding.UTF8.GetBytes(callbackUrl + "\n");
            client.Write(bytes, 0, bytes.Length);
            client.Flush();
            return true;
        }
        catch (Exception)
        {
            return false;
        }
    }

    public static async Task ServeAsync(Action<string> onCallback, CancellationToken cancellation)
    {
        while (!cancellation.IsCancellationRequested)
        {
            try
            {
                await using var server = new NamedPipeServerStream(PipeName, PipeDirection.In, 1,
                    PipeTransmissionMode.Byte, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
                await server.WaitForConnectionAsync(cancellation).ConfigureAwait(false);
                using var reader = new StreamReader(server, Encoding.UTF8);
                var line = (await reader.ReadLineAsync(cancellation).ConfigureAwait(false))?.Trim();
                if (IsOAuthCallbackUrl(line)) onCallback(line!);
            }
            catch (OperationCanceledException)
            {
                return;
            }
            catch (IOException)
            {
                // A client that disconnected early; keep serving.
            }
        }
    }
}
