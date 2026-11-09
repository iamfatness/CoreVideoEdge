using System.Buffers.Binary;
using System.IO.MemoryMappedFiles;
using System.Runtime.CompilerServices;

namespace CoreVideoEdge;

// Reads the node's per-slot preview file (docs/PROTOCOL.md "Preview frames"):
// <previewDir>/slot-<N>.cvp, memory-mapped read-only. Works the same on Windows
// and macOS (FileStream + MemoryMappedFile.CreateFromFile; no named mapping).
//
// Every value here is the node's measurement. The reader never fills a gap:
// fps and audio are null until the node measured them, and a frame is only
// copied once `frames > 0`.
public sealed record PreviewHeader(
    uint Seq,
    int Width,
    int Height,
    int Stride,
    int SourceWidth,
    int SourceHeight,
    uint Frames,
    double? Fps,
    double? AudioDb,
    uint ParticipantId,
    long HeartbeatMs,
    long LastFrameMs)
{
    public bool HasFrame => Frames > 0 && LastFrameMs != 0;

    // "Last frame N s ago" is heartbeat - lastFrame, both on the node's clock.
    public long? FrameAgeMs => HasFrame ? Math.Max(0, HeartbeatMs - LastFrameMs) : null;
}

public enum PreviewReadStatus
{
    Ok,
    Busy,      // the node was writing on every attempt; try again next tick
    Invalid,   // wrong magic, version or geometry: not a preview file we understand
}

public readonly record struct PreviewRead(PreviewReadStatus Status, PreviewHeader? Header, bool PixelsCopied, string? Error);

public sealed unsafe class PreviewFileReader : IDisposable
{
    public const int HeaderSize = 64;
    public const uint Version = 1;
    public static ReadOnlySpan<byte> Magic => "CVEP"u8;
    private const int MaxDimension = 4096;
    private const int Attempts = 4;

    private readonly MemoryMappedFile _map;
    private readonly MemoryMappedViewAccessor _view;
    private readonly long _length;
    private byte* _base;
    private bool _disposed;

    private PreviewFileReader(string path, MemoryMappedFile map, MemoryMappedViewAccessor view, long length)
    {
        Path = path;
        _map = map;
        _view = view;
        _length = length;
        byte* pointer = null;
        _view.SafeMemoryMappedViewHandle.AcquirePointer(ref pointer);
        _base = pointer + _view.PointerOffset;
    }

    public string Path { get; }

    public static string SlotFileName(int slot) => $"slot-{slot}.cvp";

    public static string SlotPath(string previewDir, int slot) => System.IO.Path.Combine(previewDir, SlotFileName(slot));

    // Opens read-only and shares read/write/delete with the node, so the node
    // can keep writing and can delete the file when the slot is unrouted.
    public static PreviewFileReader? TryOpen(string path, out string? error)
    {
        error = null;
        FileStream? stream = null;
        MemoryMappedFile? map = null;
        try
        {
            stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            var length = stream.Length;
            if (length < HeaderSize)
            {
                error = $"{path} is {length} bytes, shorter than the {HeaderSize}-byte preview header.";
                stream.Dispose();
                return null;
            }
            map = MemoryMappedFile.CreateFromFile(stream, null, 0, MemoryMappedFileAccess.Read, HandleInheritability.None, leaveOpen: false);
            var view = map.CreateViewAccessor(0, 0, MemoryMappedFileAccess.Read);
            return new PreviewFileReader(path, map, view, length);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            map?.Dispose();
            stream?.Dispose();
            error = ex.Message;
            return null;
        }
    }

    // Reads the header and, when `pixels` is given and a frame exists, copies the
    // BGRA picture into it (needs Stride * Height bytes). Honours the seqlock:
    // seq odd = node writing; the copy only counts if seq was even and unchanged.
    public PreviewRead TryRead(byte[]? pixels)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (!Magic.SequenceEqual(new ReadOnlySpan<byte>(_base, 4)))
            return new PreviewRead(PreviewReadStatus.Invalid, null, false, "Not a CoreVideo Edge preview file (magic).");
        var version = Unsafe.ReadUnaligned<uint>(_base + 4);
        if (version != Version)
            return new PreviewRead(PreviewReadStatus.Invalid, null, false, $"Preview file version {version}; this app reads version {Version}.");

        for (var attempt = 0; attempt < Attempts; ++attempt)
        {
            var seq1 = Volatile.Read(ref *(uint*)(_base + 8));
            if ((seq1 & 1) != 0)
            {
                Thread.SpinWait(64);
                continue;
            }
            Interlocked.MemoryBarrier();
            var header = ReadHeader(new ReadOnlySpan<byte>(_base, HeaderSize), seq1);
            if (header.Width <= 0 || header.Height <= 0 || header.Width > MaxDimension || header.Height > MaxDimension ||
                header.Stride < header.Width * 4 || HeaderSize + (long)header.Stride * header.Height > _length)
            {
                return new PreviewRead(PreviewReadStatus.Invalid, null, false,
                    $"Preview geometry {header.Width}x{header.Height} stride {header.Stride} does not fit the {_length}-byte file.");
            }
            var copy = pixels is not null && header.HasFrame;
            if (copy)
            {
                var bytes = header.Stride * header.Height;
                if (pixels!.Length < bytes) throw new ArgumentException($"Pixel buffer needs {bytes} bytes.", nameof(pixels));
                fixed (byte* dst = pixels) Buffer.MemoryCopy(_base + HeaderSize, dst, pixels.Length, bytes);
            }
            Interlocked.MemoryBarrier();
            var seq2 = Volatile.Read(ref *(uint*)(_base + 8));
            if (seq1 == seq2) return new PreviewRead(PreviewReadStatus.Ok, header, copy, null);
        }
        return new PreviewRead(PreviewReadStatus.Busy, null, false, null);
    }

    internal static PreviewHeader ReadHeader(ReadOnlySpan<byte> h, uint seq)
    {
        var fps = BinaryPrimitives.ReadInt32LittleEndian(h[36..]);
        var audio = BinaryPrimitives.ReadInt32LittleEndian(h[40..]);
        return new PreviewHeader(
            seq,
            (int)BinaryPrimitives.ReadUInt32LittleEndian(h[12..]),
            (int)BinaryPrimitives.ReadUInt32LittleEndian(h[16..]),
            (int)BinaryPrimitives.ReadUInt32LittleEndian(h[20..]),
            (int)BinaryPrimitives.ReadUInt32LittleEndian(h[24..]),
            (int)BinaryPrimitives.ReadUInt32LittleEndian(h[28..]),
            BinaryPrimitives.ReadUInt32LittleEndian(h[32..]),
            fps < 0 ? null : fps / 100.0,                       // -1 until measured
            audio == int.MinValue ? null : audio / 100.0,        // INT32_MIN until any audio
            BinaryPrimitives.ReadUInt32LittleEndian(h[44..]),
            BinaryPrimitives.ReadInt64LittleEndian(h[48..]),
            BinaryPrimitives.ReadInt64LittleEndian(h[56..]));
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _base = null;
        _view.SafeMemoryMappedViewHandle.ReleasePointer();
        _view.Dispose();
        _map.Dispose();
    }
}

// The heartbeat is the node's steady clock, which the app cannot compare with
// its own. "Not publishing" therefore means: the heartbeat value has not
// changed for more than `stallAfter` of the app's own time.
public sealed class PreviewHeartbeatWatch
{
    private readonly TimeSpan _stallAfter;
    private long? _lastHeartbeat;
    private TimeSpan _changedAt;

    public PreviewHeartbeatWatch(TimeSpan? stallAfter = null) => _stallAfter = stallAfter ?? TimeSpan.FromSeconds(2);

    public void Observe(long heartbeatMs, TimeSpan now)
    {
        if (_lastHeartbeat != heartbeatMs)
        {
            _lastHeartbeat = heartbeatMs;
            _changedAt = now;
        }
    }

    public bool HasObserved => _lastHeartbeat is not null;

    public TimeSpan SinceChange(TimeSpan now) => _lastHeartbeat is null ? TimeSpan.Zero : now - _changedAt;

    public bool IsStalled(TimeSpan now) => _lastHeartbeat is not null && now - _changedAt > _stallAfter;

    public void Reset() => _lastHeartbeat = null;
}
