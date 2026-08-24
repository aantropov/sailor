#nullable enable

using System.Buffers.Binary;

namespace SailorEditor.Mcp;

public sealed record McpLandscapeVegetationFormatField(
    string Name,
    int Offset,
    string Type,
    string Description);

public sealed record McpLandscapeVegetationFormat(
    string Magic,
    uint Version,
    string ByteOrder,
    uint EndianMarker,
    uint HeaderSize,
    uint ChunkRecordSize,
    uint InstanceRecordSize,
    string MatrixLayout,
    string CoordinateSpace,
    uint MaxChunksPerAxis,
    uint MaxProfileCount,
    ulong MaxInstanceCount,
    IReadOnlyList<McpLandscapeVegetationFormatField> HeaderFields,
    IReadOnlyList<McpLandscapeVegetationFormatField> ChunkFields,
    IReadOnlyList<McpLandscapeVegetationFormatField> InstanceFields);

public sealed record McpLandscapeVegetationInstance(
    string StableId,
    uint ChunkX,
    uint ChunkZ,
    uint ProfileIndex,
    uint Flags,
    bool Enabled,
    IReadOnlyList<float> Matrix,
    int LodBias,
    float CullDistanceScale,
    float ShadowDistanceScale);

public sealed record McpLandscapeVegetationSnapshot(
    bool Succeeded,
    string Message,
    string VegetationFileId,
    string AssetPath,
    McpLandscapeVegetationFormat Format,
    uint ChunksX,
    uint ChunksZ,
    float ChunkSize,
    uint ProfileCount,
    uint ChunkCount,
    ulong TotalInstanceCount,
    ulong Offset,
    int Count,
    IReadOnlyList<McpLandscapeVegetationInstance> Instances);

internal static class LandscapeVegetationBinary
{
    public const uint Version = 1;
    public const uint EndianMarker = 0x01020304;
    public const uint HeaderSize = 64;
    public const uint ChunkRecordSize = 24;
    public const uint InstanceRecordSize = 96;
    public const uint EnabledFlag = 1;
    public const uint MaxProfileCount = 1024;
    public const ulong MaxInstanceCount = 16UL * 1024UL * 1024UL;
    static readonly byte[] Magic = "SLVEG001"u8.ToArray();

    public static McpLandscapeVegetationFormat Describe() => new(
        "SLVEG001",
        Version,
        "little-endian",
        EndianMarker,
        HeaderSize,
        ChunkRecordSize,
        InstanceRecordSize,
        "16 float32 values in column-major order",
        "Landscape local space",
        64,
        MaxProfileCount,
        MaxInstanceCount,
        [
            new("magic", 0, "char[8]", "ASCII SLVEG001"),
            new("version", 8, "uint32", "Format version"),
            new("endianMarker", 12, "uint32", "0x01020304"),
            new("headerSize", 16, "uint32", "Header byte size"),
            new("chunkRecordSize", 20, "uint32", "Chunk table stride"),
            new("instanceRecordSize", 24, "uint32", "Instance table stride"),
            new("chunksX", 28, "uint32", "Landscape chunk count on X"),
            new("chunksZ", 32, "uint32", "Landscape chunk count on Z"),
            new("profileCount", 36, "uint32", "Vegetation profile count when saved"),
            new("chunkCount", 40, "uint32", "Canonical Z-major chunk record count"),
            new("instanceCount", 44, "uint64", "Total instance record count"),
            new("chunkSize", 52, "float32", "Landscape chunk size in local units"),
            new("flags", 56, "uint32", "Bit 0: matrices use Landscape local space"),
            new("reserved", 60, "uint32", "Must be zero in version 1")
        ],
        [
            new("chunkX", 0, "uint32", "Chunk X coordinate"),
            new("chunkZ", 4, "uint32", "Chunk Z coordinate"),
            new("firstInstance", 8, "uint64", "First global instance record"),
            new("instanceCount", 16, "uint32", "Records owned by this chunk"),
            new("reserved", 20, "uint32", "Must be zero in version 1")
        ],
        [
            new("stableId", 0, "uint64", "Non-zero identity unique inside the asset"),
            new("profileIndex", 8, "uint32", "Index into Landscape vegetation profiles"),
            new("flags", 12, "uint32", "Bit 0: instance is enabled"),
            new("matrix", 16, "float32[16]", "Affine local transform, column-major; its origin belongs to the owning chunk"),
            new("lodBias", 80, "int32", "Additional per-instance LOD bias"),
            new("cullDistanceScale", 84, "float32", "Main/depth cull-distance multiplier"),
            new("shadowDistanceScale", 88, "float32", "Shadow cull-distance multiplier"),
            new("reserved", 92, "uint32", "Must be zero in version 1")
        ]);

    public static McpLandscapeVegetationSnapshot ReadPage(
        string filepath,
        string fileId,
        string assetPath,
        long requestedOffset,
        int requestedLimit)
    {
        var format = Describe();
        try
        {
            using var stream = new FileStream(
                filepath,
                FileMode.Open,
                FileAccess.Read,
                FileShare.ReadWrite | FileShare.Delete);
            var header = ReadExactly(stream, checked((int)HeaderSize));
            if (!header.AsSpan(0, Magic.Length).SequenceEqual(Magic))
                throw new InvalidDataException("Landscape vegetation magic is invalid.");

            var version = U32(header, 8);
            var endian = U32(header, 12);
            var headerSize = U32(header, 16);
            var chunkStride = U32(header, 20);
            var instanceStride = U32(header, 24);
            var chunksX = U32(header, 28);
            var chunksZ = U32(header, 32);
            var profileCount = U32(header, 36);
            var chunkCount = U32(header, 40);
            var instanceCount = U64(header, 44);
            var chunkSize = F32(header, 52);
            var flags = U32(header, 56);
            var headerReserved = U32(header, 60);
            if (version != Version || endian != EndianMarker ||
                headerSize != HeaderSize || chunkStride != ChunkRecordSize ||
                instanceStride != InstanceRecordSize)
            {
                throw new InvalidDataException(
                    "Landscape vegetation header is incompatible with format version 1.");
            }
            if (chunksX == 0 || chunksZ == 0 || chunksX > 64 || chunksZ > 64 ||
                (ulong)chunksX * chunksZ != chunkCount ||
                !float.IsFinite(chunkSize) || chunkSize <= 0 ||
                flags != 1u || headerReserved != 0u ||
                profileCount > MaxProfileCount ||
                instanceCount > MaxInstanceCount)
            {
                throw new InvalidDataException("Landscape vegetation header values are invalid.");
            }

            var expectedLength = checked(
                (ulong)HeaderSize +
                (ulong)chunkCount * ChunkRecordSize +
                instanceCount * InstanceRecordSize);
            if ((ulong)stream.Length != expectedLength)
                throw new InvalidDataException("Landscape vegetation file length does not match its header.");

            var chunks = new List<(uint X, uint Z, ulong First, uint Count)>(
                checked((int)chunkCount));
            ulong expectedFirst = 0;
            for (uint chunkIndex = 0; chunkIndex < chunkCount; ++chunkIndex)
            {
                var record = ReadExactly(stream, checked((int)ChunkRecordSize));
                var chunkX = U32(record, 0);
                var chunkZ = U32(record, 4);
                var first = U64(record, 8);
                var count = U32(record, 16);
                var chunkReserved = U32(record, 20);
                if (chunkX != chunkIndex % chunksX ||
                    chunkZ != chunkIndex / chunksX ||
                    first != expectedFirst || first > instanceCount ||
                    count > instanceCount - first || chunkReserved != 0u)
                {
                    throw new InvalidDataException("Landscape vegetation chunk table is not canonical.");
                }
                chunks.Add((chunkX, chunkZ, first, count));
                expectedFirst += count;
            }
            if (expectedFirst != instanceCount)
                throw new InvalidDataException("Landscape vegetation chunk ranges are incomplete.");

            var offset = (ulong)Math.Max(requestedOffset, 0);
            offset = Math.Min(offset, instanceCount);
            var limit = Math.Clamp(requestedLimit, 1, 4096);
            var end = Math.Min(instanceCount, offset + (ulong)limit);
            var instances = new List<McpLandscapeVegetationInstance>(
                checked((int)(end - offset)));
            var instanceTableOffset = (ulong)HeaderSize +
                (ulong)chunkCount * ChunkRecordSize;
            foreach (var chunk in chunks)
            {
                var chunkEnd = chunk.First + chunk.Count;
                var first = Math.Max(offset, chunk.First);
                var last = Math.Min(end, chunkEnd);
                for (var globalIndex = first; globalIndex < last; ++globalIndex)
                {
                    stream.Position = checked((long)(instanceTableOffset +
                        globalIndex * InstanceRecordSize));
                    var record = ReadExactly(stream, checked((int)InstanceRecordSize));
                    var stableId = U64(record, 0);
                    var profileIndex = U32(record, 8);
                    var instanceFlags = U32(record, 12);
                    var instanceReserved = U32(record, 92);
                    var matrix = new float[16];
                    for (var matrixIndex = 0; matrixIndex < matrix.Length; ++matrixIndex)
                    {
                        matrix[matrixIndex] = F32(record, 16 + matrixIndex * sizeof(float));
                        if (!float.IsFinite(matrix[matrixIndex]))
                            throw new InvalidDataException("Landscape vegetation contains a non-finite matrix.");
                    }
                    const float affineEpsilon = 1.0e-5f;
                    if (MathF.Abs(matrix[3]) > affineEpsilon ||
                        MathF.Abs(matrix[7]) > affineEpsilon ||
                        MathF.Abs(matrix[11]) > affineEpsilon ||
                        MathF.Abs(matrix[15] - 1.0f) > affineEpsilon)
                    {
                        throw new InvalidDataException(
                            "Landscape vegetation instance matrices must be affine transforms.");
                    }
                    var landscapeWidth = chunksX * chunkSize;
                    var landscapeDepth = chunksZ * chunkSize;
                    var chunkMinX = chunk.X * chunkSize - landscapeWidth * 0.5f;
                    var chunkMinZ = chunk.Z * chunkSize - landscapeDepth * 0.5f;
                    var placementEpsilon = MathF.Max(0.001f, chunkSize * 0.0001f);
                    if (matrix[12] < chunkMinX - placementEpsilon ||
                        matrix[12] > chunkMinX + chunkSize + placementEpsilon ||
                        matrix[14] < chunkMinZ - placementEpsilon ||
                        matrix[14] > chunkMinZ + chunkSize + placementEpsilon)
                    {
                        throw new InvalidDataException(
                            "Landscape vegetation instance origin is outside its owning chunk.");
                    }
                    var cullScale = F32(record, 84);
                    var shadowScale = F32(record, 88);
                    if (stableId == 0 || profileIndex >= profileCount ||
                        (instanceFlags & ~EnabledFlag) != 0 || instanceReserved != 0 ||
                        !float.IsFinite(cullScale) || cullScale <= 0 ||
                        !float.IsFinite(shadowScale) || shadowScale <= 0)
                    {
                        throw new InvalidDataException("Landscape vegetation contains an invalid instance record.");
                    }
                    instances.Add(new McpLandscapeVegetationInstance(
                        $"0x{stableId:X16}",
                        chunk.X,
                        chunk.Z,
                        profileIndex,
                        instanceFlags,
                        (instanceFlags & EnabledFlag) != 0,
                        matrix,
                        I32(record, 80),
                        cullScale,
                        shadowScale));
                }
            }

            return new McpLandscapeVegetationSnapshot(
                true,
                "Landscape vegetation asset read successfully.",
                fileId,
                assetPath,
                format,
                chunksX,
                chunksZ,
                chunkSize,
                profileCount,
                chunkCount,
                instanceCount,
                offset,
                instances.Count,
                instances);
        }
        catch (Exception exception) when (
            exception is IOException or UnauthorizedAccessException or
            InvalidDataException or OverflowException)
        {
            return Failure(fileId, assetPath, exception.Message);
        }
    }

    public static McpLandscapeVegetationSnapshot Failure(
        string fileId,
        string assetPath,
        string message) => new(
            false,
            message,
            fileId,
            assetPath,
            Describe(),
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            Array.Empty<McpLandscapeVegetationInstance>());

    static byte[] ReadExactly(Stream stream, int count)
    {
        var result = new byte[count];
        stream.ReadExactly(result);
        return result;
    }

    static uint U32(byte[] bytes, int offset) =>
        BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(offset, sizeof(uint)));

    static int I32(byte[] bytes, int offset) =>
        BinaryPrimitives.ReadInt32LittleEndian(bytes.AsSpan(offset, sizeof(int)));

    static ulong U64(byte[] bytes, int offset) =>
        BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(offset, sizeof(ulong)));

    static float F32(byte[] bytes, int offset) =>
        BitConverter.Int32BitsToSingle(I32(bytes, offset));
}
