using System.Runtime.Versioning;
using CoreVideoPro.WinUI.Services;

namespace CoreVideoEdge;

// Edge-side seams for the two things the operator app does that depend on the
// operating system: protecting the Zoom tokens at rest, and making the browser's
// corevideo:// sign-in return start this app. Lifted Pro code stays verbatim;
// these pick the platform implementation around it.

// A pair of functions for FileZoomTokenStore(encrypt, decrypt).
public sealed record SecretProtection(string Name, Func<string, string> Protect, Func<string, string> Unprotect);

public static class EdgeSecretProtection
{
    public const string MacOsNotImplemented =
        "Saving Zoom sign-in tokens is not implemented on macOS yet (the Keychain store is a TODO). " +
        "Sign-in cannot be kept on this machine; nothing was written to disk.";

    // Windows: the lifted DPAPI protector (current user). Anything else: a
    // protector that refuses loudly, so tokens are never written in plain text.
    public static SecretProtection ForCurrentPlatform()
    {
        if (OperatingSystem.IsWindows()) return Dpapi();
        return Refusing(OperatingSystem.IsMacOS() ? MacOsNotImplemented
            : $"Saving Zoom sign-in tokens is not supported on {Environment.OSVersion.Platform}.");
    }

    [SupportedOSPlatform("windows")]
    private static SecretProtection Dpapi() =>
        new("DPAPI (current Windows user)", DpapiSecretProtector.Protect, DpapiSecretProtector.Unprotect);

    internal static SecretProtection Refusing(string reason) =>
        new("none", _ => throw new PlatformNotSupportedException(reason), _ => throw new PlatformNotSupportedException(reason));
}

// Registers corevideo:// (the only return URI the OAuth broker accepts) so the
// browser hands the callback to this executable.
public interface IUrlSchemeRegistrar
{
    bool TryRegister(string scheme, string exePath, out string? error);
}

public static class EdgeUrlScheme
{
    public static IUrlSchemeRegistrar ForCurrentPlatform() =>
        OperatingSystem.IsWindows() ? new WindowsUrlSchemeRegistrar() : new BundleUrlSchemeRegistrar();
}

// Windows: HKCU\Software\Classes\<scheme>, the same registration CoreVideo
// Pro's unpackaged shell writes.
[SupportedOSPlatform("windows")]
public sealed class WindowsUrlSchemeRegistrar : IUrlSchemeRegistrar
{
    public bool TryRegister(string scheme, string exePath, out string? error)
    {
        error = null;
        try
        {
            using var protocolKey = Microsoft.Win32.Registry.CurrentUser.CreateSubKey($@"Software\Classes\{scheme}");
            protocolKey.SetValue("", $"URL:{scheme} Protocol");
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
}

// macOS: a URL scheme is declared by the app bundle, not registered at run time.
// TODO(macOS): ship Info.plist with CFBundleURLTypes -> CFBundleURLSchemes
// ["corevideo"], and receive the URL through the Avalonia macOS activation
// (IActivatableLifetime / UrlsOpened) instead of a second process + named pipe.
public sealed class BundleUrlSchemeRegistrar : IUrlSchemeRegistrar
{
    public bool TryRegister(string scheme, string exePath, out string? error)
    {
        error = $"The {scheme}:// sign-in return is declared by the app bundle on this platform (Info.plist CFBundleURLTypes), " +
                "which this build does not ship yet.";
        return false;
    }
}
