using System.Runtime.Versioning;
using System.Security.Cryptography;
using System.Text;

namespace CoreVideoEdge;

// Where the protector's 256-bit key lives. macOS: one generic-password item in
// the login Keychain. Tests substitute an in-memory store.
public interface ISecretKeyStore
{
    string Description { get; }
    byte[]? Load();
    void Save(byte[] key);
}

[SupportedOSPlatform("macos")]
public sealed class MacKeychainKeyStore(string service = MacKeychainKeyStore.DefaultService) : ISecretKeyStore
{
    public const string DefaultService = "CoreVideo Edge";
    public const string Account = "token-key.v1";

    public string Description => $"macOS Keychain (\"{service}\")";
    public byte[]? Load() => MacKeychain.Read(service, Account);
    public void Save(byte[] key) => MacKeychain.Write(service, Account, key, "CoreVideo Edge sign-in key");
}

// The macOS counterpart of the lifted DpapiSecretProtector, for the lifted
// FileZoomTokenStore(encrypt, decrypt): each token is sealed with AES-256-GCM
// under a key that exists only in the Keychain, and stored as
// "keychain-aesgcm:" + base64(nonce | ciphertext | tag). Like DPAPI, the OS
// holds the key and the file holds only ciphertext.
//
// Unlike DpapiSecretProtector there is no plaintext pass-through: Edge never
// wrote a plaintext token on macOS, so a non-prefixed value is refused. Any
// Keychain failure (denied, locked, unavailable) throws; nothing falls back to
// writing the token in the clear.
public sealed class KeychainSecretProtector(ISecretKeyStore keyStore)
{
    public const string Prefix = "keychain-aesgcm:";
    private const int KeySize = 32, NonceSize = 12, TagSize = 16;
    private static readonly byte[] Aad = Encoding.UTF8.GetBytes("CoreVideoEdge.secret.v1");

    private readonly object _gate = new();
    private byte[]? _key;

    public string Description => keyStore.Description;

    public static bool IsProtected(string? value) => value is not null && value.StartsWith(Prefix, StringComparison.Ordinal);

    public string Protect(string plaintext)
    {
        if (IsProtected(plaintext)) return plaintext;
        var key = Key(create: true)!;
        var nonce = RandomNumberGenerator.GetBytes(NonceSize);
        var data = Encoding.UTF8.GetBytes(plaintext);
        var sealedBytes = new byte[NonceSize + data.Length + TagSize];
        using (var aes = new AesGcm(key, TagSize))
            aes.Encrypt(nonce, data, sealedBytes.AsSpan(NonceSize, data.Length), sealedBytes.AsSpan(NonceSize + data.Length), Aad);
        nonce.CopyTo(sealedBytes, 0);
        return Prefix + Convert.ToBase64String(sealedBytes);
    }

    public string Unprotect(string stored)
    {
        if (!IsProtected(stored))
            throw new CryptographicException("Refusing a stored sign-in value that is not Keychain-protected (plaintext is never accepted on macOS).");
        var sealedBytes = Convert.FromBase64String(stored[Prefix.Length..]);
        if (sealedBytes.Length < NonceSize + TagSize) throw new CryptographicException("Keychain-protected value is truncated.");
        var key = Key(create: false)
                  ?? throw new CryptographicException($"The sign-in key is missing from the {keyStore.Description}; sign in again.");
        var length = sealedBytes.Length - NonceSize - TagSize;
        var data = new byte[length];
        using (var aes = new AesGcm(key, TagSize))
            aes.Decrypt(sealedBytes.AsSpan(0, NonceSize), sealedBytes.AsSpan(NonceSize, length),
                sealedBytes.AsSpan(NonceSize + length), data, Aad);
        return Encoding.UTF8.GetString(data);
    }

    private byte[]? Key(bool create)
    {
        lock (_gate)
        {
            if (_key is not null) return _key;
            var loaded = keyStore.Load();
            if (loaded is null)
            {
                if (!create) return null;
                var fresh = RandomNumberGenerator.GetBytes(KeySize);
                keyStore.Save(fresh);
                // Read back: a store that accepted the write but returns something
                // else would make every token unreadable later.
                loaded = keyStore.Load();
                if (loaded is null || !loaded.AsSpan().SequenceEqual(fresh))
                    throw new CryptographicException($"The {keyStore.Description} did not return the sign-in key it just stored.");
            }
            if (loaded.Length != KeySize)
                throw new CryptographicException(
                    $"The sign-in key in the {keyStore.Description} is {loaded.Length} bytes, not {KeySize}. " +
                    "Delete the \"CoreVideo Edge\" item in Keychain Access and sign in again.");
            return _key = loaded;
        }
    }
}
