using System.Runtime.InteropServices;
using System.Runtime.Versioning;

namespace CoreVideoEdge;

// A Keychain call that did not succeed. The message carries the macOS text for
// the OSStatus (SecCopyErrorMessageString) and the number, so a denied or
// locked Keychain is reported at its cause instead of looking like "signed out".
public sealed class KeychainException(string message, int status) : Exception(message)
{
    public int Status { get; } = status;
}

// Generic-password items in the user's login Keychain (Security.framework,
// SecItem* API) through P/Invoke. No plaintext fallback anywhere: every failure
// throws KeychainException.
[SupportedOSPlatform("macos")]
public static unsafe class MacKeychain
{
    public const int ErrSecSuccess = 0;
    public const int ErrSecItemNotFound = -25300;
    public const int ErrSecDuplicateItem = -25299;

    private const string CoreFoundationLib = "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation";
    private const string SecurityLib = "/System/Library/Frameworks/Security.framework/Security";

    // Returns the item's data, or null when no such item exists.
    public static byte[]? Read(string service, string account)
    {
        using var bag = new CFBag();
        var query = bag.Dict(
            (K.SecClass, K.SecClassGenericPassword),
            (K.SecAttrService, bag.Str(service)),
            (K.SecAttrAccount, bag.Str(account)),
            (K.SecReturnData, K.CFBooleanTrue),
            (K.SecMatchLimit, K.SecMatchLimitOne));
        var status = SecItemCopyMatching(query, out var result);
        if (status == ErrSecItemNotFound) return null;
        if (status != ErrSecSuccess) throw Fail("read", service, account, status);
        try
        {
            if (result == IntPtr.Zero || CFGetTypeID(result) != CFDataGetTypeID())
                throw new KeychainException($"Keychain item {service}/{account} returned no data.", status);
            var length = (int)CFDataGetLength(result);
            var bytes = new byte[length];
            if (length > 0) Marshal.Copy(CFDataGetBytePtr(result), bytes, 0, length);
            return bytes;
        }
        finally
        {
            if (result != IntPtr.Zero) CFRelease(result);
        }
    }

    // Creates the item, or replaces its data when it already exists.
    public static void Write(string service, string account, byte[] data, string? label = null)
    {
        using var bag = new CFBag();
        var value = bag.Data(data);
        var addPairs = new List<(IntPtr, IntPtr)>
        {
            (K.SecClass, K.SecClassGenericPassword),
            (K.SecAttrService, bag.Str(service)),
            (K.SecAttrAccount, bag.Str(account)),
            (K.SecValueData, value),
        };
        if (label is not null) addPairs.Add((K.SecAttrLabel, bag.Str(label)));
        var status = SecItemAdd(bag.Dict(addPairs.ToArray()), IntPtr.Zero);
        if (status == ErrSecDuplicateItem)
        {
            var query = bag.Dict(
                (K.SecClass, K.SecClassGenericPassword),
                (K.SecAttrService, bag.Str(service)),
                (K.SecAttrAccount, bag.Str(account)));
            status = SecItemUpdate(query, bag.Dict((K.SecValueData, value)));
            if (status != ErrSecSuccess) throw Fail("update", service, account, status);
            return;
        }
        if (status != ErrSecSuccess) throw Fail("add", service, account, status);
    }

    // True when an item was deleted, false when there was none.
    public static bool Delete(string service, string account)
    {
        using var bag = new CFBag();
        var query = bag.Dict(
            (K.SecClass, K.SecClassGenericPassword),
            (K.SecAttrService, bag.Str(service)),
            (K.SecAttrAccount, bag.Str(account)));
        var status = SecItemDelete(query);
        if (status == ErrSecItemNotFound) return false;
        if (status != ErrSecSuccess) throw Fail("delete", service, account, status);
        return true;
    }

    private static KeychainException Fail(string op, string service, string account, int status) =>
        new($"Keychain {op} of \"{service}\" / \"{account}\" failed: {Describe(status)} (OSStatus {status}).", status);

    public static string Describe(int status)
    {
        var s = SecCopyErrorMessageString(status, IntPtr.Zero);
        if (s == IntPtr.Zero) return "unknown error";
        try { return ToManagedString(s); }
        finally { CFRelease(s); }
    }

    private static string ToManagedString(IntPtr cfString)
    {
        var length = (int)CFStringGetLength(cfString);
        if (length == 0) return string.Empty;
        var buffer = new char[length];
        fixed (char* p = buffer) CFStringGetCharacters(cfString, new CFRange(0, length), p);
        return new string(buffer);
    }

    // CoreFoundation objects created for one call, released together.
    private sealed class CFBag : IDisposable
    {
        private readonly List<IntPtr> _owned = [];

        public IntPtr Str(string value)
        {
            fixed (char* p = value) return Own(CFStringCreateWithCharacters(IntPtr.Zero, p, value.Length));
        }

        public IntPtr Data(byte[] value)
        {
            fixed (byte* p = value) return Own(CFDataCreate(IntPtr.Zero, p, value.Length));
        }

        public IntPtr Dict(params (IntPtr Key, IntPtr Value)[] pairs)
        {
            var keys = pairs.Select(p => p.Key).ToArray();
            var values = pairs.Select(p => p.Value).ToArray();
            fixed (IntPtr* k = keys)
            fixed (IntPtr* v = values)
                return Own(CFDictionaryCreate(IntPtr.Zero, k, v, pairs.Length, K.KeyCallBacks, K.ValueCallBacks));
        }

        private IntPtr Own(IntPtr cf)
        {
            if (cf == IntPtr.Zero) throw new KeychainException("CoreFoundation could not allocate a Keychain query.", 0);
            _owned.Add(cf);
            return cf;
        }

        public void Dispose()
        {
            for (var i = _owned.Count - 1; i >= 0; --i) CFRelease(_owned[i]);
            _owned.Clear();
        }
    }

    // Exported CFStringRef / CFBooleanRef constants and the callback structs,
    // read from the frameworks on first use (never touched off macOS).
    private static class K
    {
        private static readonly IntPtr Cf = NativeLibrary.Load(CoreFoundationLib);
        private static readonly IntPtr Sec = NativeLibrary.Load(SecurityLib);

        public static readonly IntPtr KeyCallBacks = NativeLibrary.GetExport(Cf, "kCFTypeDictionaryKeyCallBacks");
        public static readonly IntPtr ValueCallBacks = NativeLibrary.GetExport(Cf, "kCFTypeDictionaryValueCallBacks");
        public static readonly IntPtr CFBooleanTrue = Ref(Cf, "kCFBooleanTrue");

        public static readonly IntPtr SecClass = Ref(Sec, "kSecClass");
        public static readonly IntPtr SecClassGenericPassword = Ref(Sec, "kSecClassGenericPassword");
        public static readonly IntPtr SecAttrService = Ref(Sec, "kSecAttrService");
        public static readonly IntPtr SecAttrAccount = Ref(Sec, "kSecAttrAccount");
        public static readonly IntPtr SecAttrLabel = Ref(Sec, "kSecAttrLabel");
        public static readonly IntPtr SecValueData = Ref(Sec, "kSecValueData");
        public static readonly IntPtr SecReturnData = Ref(Sec, "kSecReturnData");
        public static readonly IntPtr SecMatchLimit = Ref(Sec, "kSecMatchLimit");
        public static readonly IntPtr SecMatchLimitOne = Ref(Sec, "kSecMatchLimitOne");

        private static IntPtr Ref(IntPtr lib, string name) => Marshal.ReadIntPtr(NativeLibrary.GetExport(lib, name));
    }

    [StructLayout(LayoutKind.Sequential)]
    private readonly struct CFRange(nint location, nint length)
    {
        public readonly nint Location = location;
        public readonly nint Length = length;
    }

    [DllImport(CoreFoundationLib)] private static extern IntPtr CFStringCreateWithCharacters(IntPtr alloc, char* chars, nint numChars);
    [DllImport(CoreFoundationLib)] private static extern nint CFStringGetLength(IntPtr s);
    [DllImport(CoreFoundationLib)] private static extern void CFStringGetCharacters(IntPtr s, CFRange range, char* buffer);
    [DllImport(CoreFoundationLib)] private static extern IntPtr CFDataCreate(IntPtr alloc, byte* bytes, nint length);
    [DllImport(CoreFoundationLib)] private static extern nint CFDataGetLength(IntPtr data);
    [DllImport(CoreFoundationLib)] private static extern IntPtr CFDataGetBytePtr(IntPtr data);
    [DllImport(CoreFoundationLib)] private static extern nuint CFDataGetTypeID();
    [DllImport(CoreFoundationLib)] private static extern nuint CFGetTypeID(IntPtr cf);
    [DllImport(CoreFoundationLib)]
    private static extern IntPtr CFDictionaryCreate(IntPtr alloc, IntPtr* keys, IntPtr* values, nint count, IntPtr keyCallBacks, IntPtr valueCallBacks);
    [DllImport(CoreFoundationLib)] private static extern void CFRelease(IntPtr cf);

    [DllImport(SecurityLib)] private static extern int SecItemAdd(IntPtr attributes, IntPtr result);
    [DllImport(SecurityLib)] private static extern int SecItemCopyMatching(IntPtr query, out IntPtr result);
    [DllImport(SecurityLib)] private static extern int SecItemUpdate(IntPtr query, IntPtr attributesToUpdate);
    [DllImport(SecurityLib)] private static extern int SecItemDelete(IntPtr query);
    [DllImport(SecurityLib)] private static extern IntPtr SecCopyErrorMessageString(int status, IntPtr reserved);
}
