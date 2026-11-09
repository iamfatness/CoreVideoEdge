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
    // Windows: the lifted DPAPI protector (current user). macOS: AES-GCM under a
    // key held in the login Keychain. Anything else: a protector that refuses
    // loudly, so tokens are never written in plain text.
    public static SecretProtection ForCurrentPlatform()
    {
        if (OperatingSystem.IsWindows()) return Dpapi();
        if (OperatingSystem.IsMacOS()) return Keychain();
        return Refusing($"Saving Zoom sign-in tokens is not supported on {Environment.OSVersion.Platform}.");
    }

    [SupportedOSPlatform("windows")]
    private static SecretProtection Dpapi() =>
        new("DPAPI (current Windows user)", DpapiSecretProtector.Protect, DpapiSecretProtector.Unprotect);

    [SupportedOSPlatform("macos")]
    private static SecretProtection Keychain()
    {
        var protector = new KeychainSecretProtector(new MacKeychainKeyStore());
        return new(protector.Description, protector.Protect, protector.Unprotect);
    }

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
        OperatingSystem.IsWindows() ? new WindowsUrlSchemeRegistrar()
        : OperatingSystem.IsMacOS() ? new MacUrlSchemeRegistrar()
        : new UnsupportedUrlSchemeRegistrar();
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

// Neither Windows nor macOS: there is no supported way to receive the return.
public sealed class UnsupportedUrlSchemeRegistrar : IUrlSchemeRegistrar
{
    public bool TryRegister(string scheme, string exePath, out string? error)
    {
        error = $"The {scheme}:// sign-in return is not supported on {Environment.OSVersion.Platform}.";
        return false;
    }
}
