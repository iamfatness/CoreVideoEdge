using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Text;
using System.Xml.Linq;

namespace CoreVideoEdge;

// The app bundle around an executable (…/X.app/Contents/MacOS/<exe>) and what
// its Info.plist declares. Pure file reading: testable on any OS.
public static class MacAppBundle
{
    // The .app directory that contains exePath as its Contents/MacOS executable, or null.
    public static string? FindBundle(string exePath)
    {
        var macOsDir = Path.GetDirectoryName(Path.GetFullPath(exePath));
        if (macOsDir is null || Path.GetFileName(macOsDir) != "MacOS") return null;
        var contents = Path.GetDirectoryName(macOsDir);
        if (contents is null || Path.GetFileName(contents) != "Contents") return null;
        var bundle = Path.GetDirectoryName(contents);
        return bundle is not null && bundle.EndsWith(".app", StringComparison.OrdinalIgnoreCase) ? bundle : null;
    }

    public static string InfoPlistPath(string bundle) => Path.Combine(bundle, "Contents", "Info.plist");

    // The value following <key>name</key> in the top-level dict of an XML plist.
    public static XElement? Value(XDocument plist, string key)
    {
        var dict = plist.Root?.Element("dict");
        var keyElement = dict?.Elements("key").FirstOrDefault(k => k.Value == key);
        return keyElement?.ElementsAfterSelf().FirstOrDefault();
    }

    public static string? BundleIdentifier(XDocument plist) => Value(plist, "CFBundleIdentifier")?.Value;

    // True when CFBundleURLTypes lists the scheme in some CFBundleURLSchemes array.
    public static bool DeclaresScheme(XDocument plist, string scheme)
    {
        var types = Value(plist, "CFBundleURLTypes");
        if (types?.Name != "array") return false;
        foreach (var type in types.Elements("dict"))
        {
            var key = type.Elements("key").FirstOrDefault(k => k.Value == "CFBundleURLSchemes");
            var schemes = key?.ElementsAfterSelf().FirstOrDefault();
            if (schemes?.Name == "array" &&
                schemes.Elements("string").Any(s => string.Equals(s.Value, scheme, StringComparison.OrdinalIgnoreCase)))
                return true;
        }
        return false;
    }
}

// macOS: corevideo:// is declared by the bundle's Info.plist (CFBundleURLTypes);
// the browser's return arrives as an Apple Event, which Avalonia surfaces as
// ProtocolActivatedEventArgs (App.axaml.cs). At Sign in this verifies the
// declaration, registers the bundle with Launch Services and makes it the
// default corevideo:// handler, the Mac counterpart of Windows' HKCU write: on
// a Mac that also runs CoreVideo Pro, the most recent sign-in owns the scheme.
[SupportedOSPlatform("macos")]
public sealed class MacUrlSchemeRegistrar : IUrlSchemeRegistrar
{
    public bool TryRegister(string scheme, string exePath, out string? error)
    {
        error = null;
        var bundle = MacAppBundle.FindBundle(exePath);
        if (bundle is null)
        {
            error = $"This copy is not running from CoreVideo Edge.app ({exePath}); the {scheme}:// return is declared by the app bundle. " +
                    "Start the app from CoreVideo Edge.app.";
            return false;
        }
        XDocument plist;
        try
        {
            plist = XDocument.Load(MacAppBundle.InfoPlistPath(bundle));
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or System.Xml.XmlException)
        {
            error = $"Could not read {MacAppBundle.InfoPlistPath(bundle)}: {ex.Message}";
            return false;
        }
        if (!MacAppBundle.DeclaresScheme(plist, scheme))
        {
            error = $"{MacAppBundle.InfoPlistPath(bundle)} does not declare {scheme}:// in CFBundleURLTypes.";
            return false;
        }
        var bundleId = MacAppBundle.BundleIdentifier(plist);
        if (string.IsNullOrEmpty(bundleId))
        {
            error = $"{MacAppBundle.InfoPlistPath(bundle)} has no CFBundleIdentifier.";
            return false;
        }
        var status = LaunchServices.Register(bundle);
        if (status != 0)
        {
            error = $"Launch Services could not register {bundle} (OSStatus {status}).";
            return false;
        }
        status = LaunchServices.SetDefaultHandler(scheme, bundleId);
        if (status != 0)
        {
            error = $"Launch Services could not make {bundleId} the {scheme}:// handler (OSStatus {status}).";
            return false;
        }
        return true;
    }

    private static unsafe class LaunchServices
    {
        private const string CoreFoundationLib = "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation";
        private const string CoreServicesLib = "/System/Library/Frameworks/CoreServices.framework/CoreServices";

        public static int Register(string bundlePath)
        {
            var path = Encoding.UTF8.GetBytes(bundlePath);
            IntPtr url;
            fixed (byte* p = path) url = CFURLCreateFromFileSystemRepresentation(IntPtr.Zero, p, path.Length, 1);
            if (url == IntPtr.Zero) return -1;
            try { return LSRegisterURL(url, 1); }
            finally { CFRelease(url); }
        }

        public static int SetDefaultHandler(string scheme, string bundleId)
        {
            var s = Str(scheme);
            var b = Str(bundleId);
            try { return LSSetDefaultHandlerForURLScheme(s, b); }
            finally { CFRelease(s); CFRelease(b); }
        }

        private static IntPtr Str(string value)
        {
            fixed (char* p = value) return CFStringCreateWithCharacters(IntPtr.Zero, p, value.Length);
        }

        [DllImport(CoreFoundationLib)] private static extern IntPtr CFStringCreateWithCharacters(IntPtr alloc, char* chars, nint numChars);
        [DllImport(CoreFoundationLib)] private static extern IntPtr CFURLCreateFromFileSystemRepresentation(IntPtr alloc, byte* buffer, nint length, byte isDirectory);
        [DllImport(CoreFoundationLib)] private static extern void CFRelease(IntPtr cf);
        [DllImport(CoreServicesLib)] private static extern int LSRegisterURL(IntPtr url, byte update);
        [DllImport(CoreServicesLib)] private static extern int LSSetDefaultHandlerForURLScheme(IntPtr scheme, IntPtr bundleId);
    }
}
