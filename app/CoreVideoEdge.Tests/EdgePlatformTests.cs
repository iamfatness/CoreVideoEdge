using System.Security.Cryptography;
using System.Xml.Linq;
using CoreVideoEdge;
using CoreVideoEdge.Desktop.Controls;
using Avalonia.Input;
using Xunit;

namespace CoreVideoEdge.Tests;

// Runs only on macOS (CI: the macos-15 job); skipped elsewhere.
public sealed class MacOnlyFactAttribute : FactAttribute
{
    public MacOnlyFactAttribute()
    {
        if (!OperatingSystem.IsMacOS()) Skip = "macOS only (runs in the macOS workflow)";
    }
}

public sealed class EdgePlatformTests
{
    [Fact]
    public void CurrentPlatformProtectorRoundTripsOrRefusesLoudly()
    {
        var p = EdgeSecretProtection.ForCurrentPlatform();
        if (OperatingSystem.IsWindows() || OperatingSystem.IsMacOS())
        {
            var stored = p.Protect("secret");
            Assert.NotEqual("secret", stored);
            Assert.DoesNotContain("secret", stored);
            Assert.Equal("secret", p.Unprotect(stored));
        }
        else
        {
            Assert.Throws<PlatformNotSupportedException>(() => p.Protect("secret"));
        }
    }

    [Fact]
    public void RefusingProtectorNeverReturnsPlaintext()
    {
        var p = EdgeSecretProtection.Refusing("nope");
        var ex = Assert.Throws<PlatformNotSupportedException>(() => p.Protect("secret"));
        Assert.Equal("nope", ex.Message);
        Assert.Throws<PlatformNotSupportedException>(() => p.Unprotect("secret"));
    }

    [MacOnlyFact]
    public void MacUsesTheKeychain()
    {
        var p = EdgeSecretProtection.ForCurrentPlatform();
        Assert.Contains("Keychain", p.Name);
        Assert.StartsWith(KeychainSecretProtector.Prefix, p.Protect("secret"));
    }

    [MacOnlyFact]
    public void MacKeychainAddReadUpdateDelete()
    {
        var service = $"CoreVideo Edge Tests {Guid.NewGuid():N}";
        try
        {
            Assert.Null(MacKeychain.Read(service, "a"));
            MacKeychain.Write(service, "a", [1, 2, 3], "test item");
            Assert.Equal(new byte[] { 1, 2, 3 }, MacKeychain.Read(service, "a"));
            MacKeychain.Write(service, "a", [4, 5]);
            Assert.Equal(new byte[] { 4, 5 }, MacKeychain.Read(service, "a"));
            Assert.Null(MacKeychain.Read(service, "b"));
            Assert.True(MacKeychain.Delete(service, "a"));
            Assert.False(MacKeychain.Delete(service, "a"));
            Assert.Null(MacKeychain.Read(service, "a"));
        }
        finally
        {
            MacKeychain.Delete(service, "a");
        }
    }

    [MacOnlyFact]
    public void MacKeychainKeyStoreBacksTheProtector()
    {
        var service = $"CoreVideo Edge Tests {Guid.NewGuid():N}";
        try
        {
            var stored = new KeychainSecretProtector(new MacKeychainKeyStore(service)).Protect("refresh-token");
            Assert.Equal(32, MacKeychain.Read(service, MacKeychainKeyStore.Account)!.Length);
            // A fresh protector (a new app run) reads the same key back.
            Assert.Equal("refresh-token", new KeychainSecretProtector(new MacKeychainKeyStore(service)).Unprotect(stored));
            MacKeychain.Delete(service, MacKeychainKeyStore.Account);
            Assert.Throws<CryptographicException>(() => new KeychainSecretProtector(new MacKeychainKeyStore(service)).Unprotect(stored));
        }
        finally
        {
            MacKeychain.Delete(service, MacKeychainKeyStore.Account);
        }
    }

    [MacOnlyFact]
    public void MacKeychainErrorTextComesFromTheOs()
    {
        Assert.False(string.IsNullOrWhiteSpace(MacKeychain.Describe(MacKeychain.ErrSecItemNotFound)));
    }

    [Fact]
    public void UrlSchemeRegistrarPerPlatform()
    {
        var registrar = EdgeUrlScheme.ForCurrentPlatform();
        if (OperatingSystem.IsWindows()) Assert.IsType<WindowsUrlSchemeRegistrar>(registrar);
        else if (OperatingSystem.IsMacOS()) Assert.IsType<MacUrlSchemeRegistrar>(registrar);
        else Assert.IsType<UnsupportedUrlSchemeRegistrar>(registrar);
    }

    [MacOnlyFact]
    public void MacRegistrarRefusesOutsideAnAppBundle()
    {
        Assert.False(new MacUrlSchemeRegistrar().TryRegister("corevideo", "/usr/local/bin/CoreVideoEdge", out var error));
        Assert.Contains("CoreVideo Edge.app", error);
    }

    [Fact]
    public void FindsTheBundleAroundItsExecutable()
    {
        var root = Path.GetFullPath(Path.Combine(Path.GetTempPath(), "x"));
        var exe = Path.Combine(root, "CoreVideo Edge.app", "Contents", "MacOS", "CoreVideoEdge");
        Assert.Equal(Path.Combine(root, "CoreVideo Edge.app"), MacAppBundle.FindBundle(exe));
        Assert.Null(MacAppBundle.FindBundle(Path.Combine(root, "bin", "CoreVideoEdge")));
        Assert.Null(MacAppBundle.FindBundle(Path.Combine(root, "NotABundle", "Contents", "MacOS", "CoreVideoEdge")));
    }

    private const string Plist = """
        <?xml version="1.0" encoding="UTF-8"?>
        <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
        <plist version="1.0">
        <dict>
          <key>CFBundleIdentifier</key><string>us.iamfatness.corevideo.edge</string>
          <key>CFBundleURLTypes</key>
          <array>
            <dict>
              <key>CFBundleURLName</key><string>us.iamfatness.corevideo.edge.oauth</string>
              <key>CFBundleURLSchemes</key><array><string>corevideo</string></array>
            </dict>
          </array>
        </dict>
        </plist>
        """;

    [Fact]
    public void ReadsTheSchemeAndIdentifierFromInfoPlist()
    {
        var doc = XDocument.Parse(Plist);
        Assert.True(MacAppBundle.DeclaresScheme(doc, "corevideo"));
        Assert.False(MacAppBundle.DeclaresScheme(doc, "zoommtg"));
        Assert.Equal("us.iamfatness.corevideo.edge", MacAppBundle.BundleIdentifier(doc));
        Assert.False(MacAppBundle.DeclaresScheme(XDocument.Parse("<plist><dict/></plist>"), "corevideo"));
    }

    // The packaging script writes the real Info.plist; it must say what this test reads.
    [Fact]
    public void PackagingScriptDeclaresTheScheme()
    {
        var script = File.ReadAllText(Path.Combine(AppContext.BaseDirectory, "Fixtures", "package-macos-app.sh"));
        Assert.Contains("<key>CFBundleURLSchemes</key>", script);
        Assert.Contains("<string>corevideo</string>", script);
        Assert.Contains("us.iamfatness.corevideo.edge", script);
    }

    [Fact]
    public void ShortcutsUseCommandOnMacAndCtrlOnWindows()
    {
        Assert.Equal("Ctrl+1", Shortcuts.Text("Ctrl+1", mac: false));
        Assert.Equal("⌘1", Shortcuts.Text("Ctrl+1", mac: true));
        Assert.Equal("⇧⌘R", Shortcuts.Text("Ctrl+Shift+R", mac: true));
        var g = Shortcuts.ForPlatform(new KeyGesture(Key.R, KeyModifiers.Control | KeyModifiers.Shift), mac: true);
        Assert.Equal(KeyModifiers.Meta | KeyModifiers.Shift, g.KeyModifiers);
        Assert.Equal(Key.R, g.Key);
        var f5 = new KeyGesture(Key.F5);
        Assert.Same(f5, Shortcuts.ForPlatform(f5, mac: true));
        var win = new KeyGesture(Key.D1, KeyModifiers.Control);
        Assert.Same(win, Shortcuts.ForPlatform(win, mac: false));
    }
}

public sealed class KeychainSecretProtectorTests
{
    private sealed class MemoryKeyStore : ISecretKeyStore
    {
        public byte[]? Key;
        public int Saves;
        public Exception? FailWith;
        public string Description => "memory";
        public byte[]? Load() => FailWith is null ? Key?.ToArray() : throw FailWith;
        public void Save(byte[] key) { if (FailWith is not null) throw FailWith; Key = key.ToArray(); ++Saves; }
    }

    [Fact]
    public void RoundTripsWithoutPlaintextAndCreatesTheKeyOnce()
    {
        var store = new MemoryKeyStore();
        var p = new KeychainSecretProtector(store);
        var a = p.Protect("access-token-value");
        var b = p.Protect("access-token-value");
        Assert.StartsWith(KeychainSecretProtector.Prefix, a);
        Assert.DoesNotContain("access-token-value", a);
        Assert.NotEqual(a, b);  // fresh nonce each time
        Assert.Equal("access-token-value", p.Unprotect(a));
        Assert.Equal("access-token-value", new KeychainSecretProtector(store).Unprotect(b));
        Assert.Equal(1, store.Saves);
        Assert.Equal(32, store.Key!.Length);
        Assert.Equal(a, p.Protect(a));  // idempotent, like DPAPI
    }

    [Fact]
    public void RefusesPlaintextTamperingAndAnotherKey()
    {
        var store = new MemoryKeyStore();
        var p = new KeychainSecretProtector(store);
        Assert.Throws<CryptographicException>(() => p.Unprotect("plain-token"));
        var stored = p.Protect("token");
        var bytes = Convert.FromBase64String(stored[KeychainSecretProtector.Prefix.Length..]);
        bytes[^1] ^= 1;
        Assert.ThrowsAny<CryptographicException>(() => p.Unprotect(KeychainSecretProtector.Prefix + Convert.ToBase64String(bytes)));
        var other = new KeychainSecretProtector(new MemoryKeyStore());
        other.Protect("x");
        Assert.ThrowsAny<CryptographicException>(() => other.Unprotect(stored));
    }

    [Fact]
    public void MissingKeyOnReadDoesNotCreateOne()
    {
        var store = new MemoryKeyStore();
        var stored = new KeychainSecretProtector(store).Protect("token");
        store.Key = null;
        var ex = Assert.Throws<CryptographicException>(() => new KeychainSecretProtector(store).Unprotect(stored));
        Assert.Contains("missing", ex.Message);
        Assert.Null(store.Key);
    }

    [Fact]
    public void StoreFailureIsLoudAndNeverFallsBackToPlaintext()
    {
        var store = new MemoryKeyStore { FailWith = new InvalidOperationException("Keychain denied") };
        var p = new KeychainSecretProtector(store);
        Assert.Equal("Keychain denied", Assert.Throws<InvalidOperationException>(() => p.Protect("token")).Message);
    }

    [Fact]
    public void WrongSizeKeyIsRefusedNotReplaced()
    {
        var store = new MemoryKeyStore { Key = new byte[5] };
        var ex = Assert.Throws<CryptographicException>(() => new KeychainSecretProtector(store).Protect("token"));
        Assert.Contains("Keychain Access", ex.Message);
        Assert.Equal(5, store.Key!.Length);
    }

    [Fact]
    public void StoreThatLosesTheKeyIsCaught()
    {
        var store = new ForgetfulStore();
        Assert.Throws<CryptographicException>(() => new KeychainSecretProtector(store).Protect("token"));
    }

    private sealed class ForgetfulStore : ISecretKeyStore
    {
        public string Description => "forgetful";
        public byte[]? Load() => null;
        public void Save(byte[] key) { }
    }
}

public sealed class OAuthCallbackInboxTests
{
    [Fact]
    public void HoldsEarlyCallbacksAndDeliversInOrder()
    {
        var inbox = new OAuthCallbackInbox();
        Assert.True(inbox.Deliver("corevideo://oauth/callback?code=1&state=a"));
        Assert.False(inbox.Deliver("https://example.com"));
        Assert.False(inbox.Deliver(null));
        var got = new List<string>();
        inbox.Attach(got.Add);
        Assert.Equal(["corevideo://oauth/callback?code=1&state=a"], got);
        inbox.Deliver("corevideo://oauth/callback?code=2&state=b");
        Assert.Equal(2, got.Count);
    }
}
