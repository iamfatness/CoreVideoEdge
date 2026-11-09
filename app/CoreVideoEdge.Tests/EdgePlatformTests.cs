using CoreVideoEdge;
using Xunit;

namespace CoreVideoEdge.Tests;

public sealed class EdgePlatformTests
{
    [Fact]
    public void CurrentPlatformProtectorRoundTripsOrRefusesLoudly()
    {
        var p = EdgeSecretProtection.ForCurrentPlatform();
        if (OperatingSystem.IsWindows())
        {
            var stored = p.Protect("secret");
            Assert.NotEqual("secret", stored);
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

    [Fact]
    public void BundleRegistrarExplainsTheMacRoute()
    {
        Assert.False(new BundleUrlSchemeRegistrar().TryRegister("corevideo", "/x", out var error));
        Assert.Contains("CFBundleURLTypes", error);
    }
}
