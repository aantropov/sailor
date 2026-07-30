using System.Buffers.Binary;
using SailorEditor.Workspace;

namespace SailorEditor.Services;

public static class ModelMiniaturePath
{
    public const string DirectoryName = "Models";
    public const string FileExtension = ".png";
    public const int Resolution = 256;

    public static bool TryResolve(
        string? cacheDirectory,
        string? fileId,
        out string miniaturePath)
    {
        miniaturePath = string.Empty;
        if (string.IsNullOrWhiteSpace(cacheDirectory) ||
            string.IsNullOrWhiteSpace(fileId) ||
            (!Guid.TryParseExact(fileId, "B", out _) &&
                !Guid.TryParseExact(fileId, "D", out _)))
        {
            return false;
        }

        try
        {
            var cacheRoot =
                WorkspacePathPolicy.NormalizePhysicalPath(cacheDirectory);
            var candidate = Path.GetFullPath(
                Path.Combine(
                    cacheRoot,
                    DirectoryName,
                    fileId + FileExtension));

            if (!WorkspacePathPolicy.IsInsideRoot(cacheRoot, candidate))
            {
                return false;
            }

            miniaturePath = candidate;
            return true;
        }
        catch
        {
            return false;
        }
    }
}

public static class ModelMiniatureLoader
{
    const long MaxFileSize = 16 * 1024 * 1024;
    static ReadOnlySpan<byte> PngSignature =>
    [
        0x89, 0x50, 0x4e, 0x47,
        0x0d, 0x0a, 0x1a, 0x0a
    ];
    static ReadOnlySpan<byte> HeaderChunkType => "IHDR"u8;
    static ReadOnlySpan<byte> EndChunk => "IEND"u8;

    public static bool TryLoad(
        string? cacheDirectory,
        string? fileId,
        out byte[] miniatureBytes)
    {
        miniatureBytes = [];
        if (!ModelMiniaturePath.TryResolve(
                cacheDirectory,
                fileId,
                out var miniaturePath))
        {
            return false;
        }

        try
        {
            using var stream = new FileStream(
                miniaturePath,
                FileMode.Open,
                FileAccess.Read,
                FileShare.ReadWrite | FileShare.Delete,
                bufferSize: 4096,
                FileOptions.SequentialScan);
            if (stream.Length <= 0 || stream.Length > MaxFileSize)
            {
                return false;
            }

            var bytes = new byte[(int)stream.Length];
            stream.ReadExactly(bytes);
            if (!HasExpectedPngEnvelope(bytes))
            {
                return false;
            }

            miniatureBytes = bytes;
            return true;
        }
        catch (Exception exception)
            when (exception is IOException
                or UnauthorizedAccessException
                or NotSupportedException
                or ArgumentException)
        {
            return false;
        }
    }

    static bool HasExpectedPngEnvelope(ReadOnlySpan<byte> bytes)
    {
        const int pngHeaderLength = 24;
        const int pngEndChunkLength = 12;
        if (bytes.Length < pngHeaderLength + pngEndChunkLength ||
            !bytes[..PngSignature.Length].SequenceEqual(PngSignature) ||
            BinaryPrimitives.ReadUInt32BigEndian(bytes.Slice(8, 4)) != 13 ||
            !bytes.Slice(12, 4).SequenceEqual(HeaderChunkType))
        {
            return false;
        }

        var width =
            BinaryPrimitives.ReadUInt32BigEndian(bytes.Slice(16, 4));
        var height =
            BinaryPrimitives.ReadUInt32BigEndian(bytes.Slice(20, 4));
        var endChunkOffset = bytes.Length - pngEndChunkLength;
        return width == ModelMiniaturePath.Resolution &&
            height == ModelMiniaturePath.Resolution &&
            BinaryPrimitives.ReadUInt32BigEndian(
                bytes.Slice(endChunkOffset, 4)) == 0 &&
            bytes.Slice(endChunkOffset + 4, 4).SequenceEqual(EndChunk);
    }
}
