using System.Buffers.Binary;
using CoreVideoEdge;

namespace CoreVideoEdge.Tests;

// TEST FIXTURE ONLY. Writes a preview file in the documented layout
// (docs/PROTOCOL.md "Preview frames") so the reader can be tested without the
// node, and so screenshot fixtures can be made while the node's preview
// publisher is not available. Never referenced by the product.
public sealed class PreviewFileWriter : IDisposable
{
    public const int Width = 480, Height = 270, Stride = Width * 4;
    private readonly FileStream _file;

    public PreviewFileWriter(string path)
    {
        Path = path;
        _file = new FileStream(path, FileMode.Create, FileAccess.ReadWrite, FileShare.ReadWrite | FileShare.Delete);
        _file.SetLength(PreviewFileReader.HeaderSize + Stride * Height);
        var header = new byte[PreviewFileReader.HeaderSize];
        "CVEP"u8.CopyTo(header);
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(4), 1);
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(12), Width);
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(16), Height);
        BinaryPrimitives.WriteUInt32LittleEndian(header.AsSpan(20), Stride);
        BinaryPrimitives.WriteInt32LittleEndian(header.AsSpan(36), -1);
        BinaryPrimitives.WriteInt32LittleEndian(header.AsSpan(40), int.MinValue);
        WriteAt(0, header);
    }

    public string Path { get; }

    public void SetSeq(uint seq) => WriteU32(8, seq);

    public void Publish(uint seq, int srcW, int srcH, uint frames, int fpsX100, int audioDbX100, uint participant,
        long heartbeat, long lastFrame, byte[]? pixels)
    {
        WriteU32(8, seq | 1);                 // writing
        WriteU32(24, (uint)srcW);
        WriteU32(28, (uint)srcH);
        WriteU32(32, frames);
        WriteI32(36, fpsX100);
        WriteI32(40, audioDbX100);
        WriteU32(44, participant);
        WriteI64(48, heartbeat);
        WriteI64(56, lastFrame);
        if (pixels is not null) WriteAt(PreviewFileReader.HeaderSize, pixels);
        WriteU32(8, seq & ~1u);               // done (even)
        _file.Flush();
    }

    // A simple, obviously synthetic test card: vertical bars plus a label band.
    public static byte[] TestCard(byte r, byte g, byte b)
    {
        var px = new byte[Stride * Height];
        for (var y = 0; y < Height; ++y)
        for (var x = 0; x < Width; ++x)
        {
            var i = y * Stride + x * 4;
            var bar = x / (Width / 8);
            var shade = (byte)(40 + bar * 24);
            var band = y > Height * 3 / 4;
            px[i + 0] = band ? (byte)20 : (byte)(b * shade / 255);
            px[i + 1] = band ? (byte)20 : (byte)(g * shade / 255);
            px[i + 2] = band ? (byte)20 : (byte)(r * shade / 255);
            px[i + 3] = 255;
        }
        return px;
    }

    private void WriteU32(long at, uint v) { Span<byte> b = stackalloc byte[4]; BinaryPrimitives.WriteUInt32LittleEndian(b, v); WriteAt(at, b); }
    private void WriteI32(long at, int v) { Span<byte> b = stackalloc byte[4]; BinaryPrimitives.WriteInt32LittleEndian(b, v); WriteAt(at, b); }
    private void WriteI64(long at, long v) { Span<byte> b = stackalloc byte[8]; BinaryPrimitives.WriteInt64LittleEndian(b, v); WriteAt(at, b); }

    private void WriteAt(long at, ReadOnlySpan<byte> bytes)
    {
        _file.Position = at;
        _file.Write(bytes);
        _file.Flush();
    }

    public void Dispose() => _file.Dispose();
}
