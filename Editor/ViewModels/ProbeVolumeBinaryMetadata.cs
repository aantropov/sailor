using System.Buffers.Binary;
using System.Numerics;
using System.Text;
using SailorEditor.Services;

namespace SailorEditor.ViewModels;

internal sealed record ProbeVolumeBinaryMetadata(
    uint FormatVersion,
    uint BakedStateCount,
    uint SphericalHarmonicsOrder,
    uint Compression,
    ulong LayoutHash,
    ulong RepresentationHash,
    ulong TransportHash,
    ulong LightingHash,
    ulong SourceWorldHash,
    Vector3 VolumeMin,
    Vector3 VolumeMax,
    uint RaysPerProbe,
    uint BounceCount,
    uint RandomSeed,
    uint MaxSubdivisionLevel,
    float MinProbeSpacing,
    float NormalBias,
    float ViewBias,
    float MaxRayDistance,
    float SkyIndirectIntensity,
    bool IncludeSky,
    bool IncludeEmissive,
    bool IncludeDirectLighting,
    uint BrickCount,
    uint ProbeCount,
    uint InvalidProbeCount,
    uint RelocatedProbeCount,
    float AverageValidity,
    float BakeDurationSeconds,
    string StateName,
    string BakerVersion,
    string Diagnostic,
    ulong PayloadChecksum,
    long FileBytes)
{
    const uint SupportedFormatVersion = 1;
    const uint SupportedSphericalHarmonicsOrder = 2;
    const uint SupportedCompression = 0;
    const uint EndianMarker = 0x01020304;
    const uint FixedHeaderSize = 40;
    const uint MaxMetadataStringBytes = 1024 * 1024;
    const uint MaxBrickCount = 1024 * 1024;
    const uint MaxProbeCount = 16 * 1024 * 1024;
    const ulong MaxPayloadBytes = 8UL * 1024 * 1024 * 1024;
    const ulong BrickRecordBytes = 48;
    const ulong ProbeRecordBytes = 212;
    static ReadOnlySpan<byte> Magic => "SLRPROBE"u8;

    public static ProbeVolumeBinaryMetadata Read(FileInfo asset)
    {
        ArgumentNullException.ThrowIfNull(asset);
        using var stream = new FileStream(
            asset.FullName,
            FileMode.Open,
            FileAccess.Read,
            FileShare.ReadWrite | FileShare.Delete);
        if (stream.Length < FixedHeaderSize)
            throw new InvalidDataException("The .probes file is smaller than its fixed header.");

        Span<byte> header = stackalloc byte[(int)FixedHeaderSize];
        stream.ReadExactly(header);
        if (!header[..Magic.Length].SequenceEqual(Magic))
            throw new InvalidDataException("The .probes magic value is invalid.");

        var formatVersion = ReadUInt32(header, 8);
        var endian = ReadUInt32(header, 12);
        var headerSize = ReadUInt32(header, 16);
        var headerFlags = ReadUInt32(header, 20);
        var payloadBytes = ReadUInt64(header, 24);
        var payloadChecksum = ReadUInt64(header, 32);
        if (formatVersion != SupportedFormatVersion)
            throw new InvalidDataException("The .probes format version is unsupported.");
        if (endian != EndianMarker)
            throw new InvalidDataException("The .probes file is not little-endian.");
        if (headerFlags != 0)
            throw new InvalidDataException("The .probes fixed header contains unsupported flags.");
        if (headerSize != FixedHeaderSize ||
            payloadBytes > MaxPayloadBytes ||
            payloadBytes != checked((ulong)stream.Length - FixedHeaderSize))
        {
            throw new InvalidDataException("The .probes payload size does not match the file size.");
        }
        if (CalculateRemainingChecksum(stream) != payloadChecksum)
            throw new InvalidDataException("The .probes payload checksum does not match.");

        stream.Position = FixedHeaderSize;
        using var reader = new BinaryReader(stream, Encoding.UTF8, leaveOpen: true);
        var bakedStateCount = reader.ReadUInt32();
        var shOrder = reader.ReadUInt32();
        var compression = reader.ReadUInt32();
        var payloadFlags = reader.ReadUInt32();
        var layoutHash = reader.ReadUInt64();
        var representationHash = reader.ReadUInt64();
        var transportHash = reader.ReadUInt64();
        var lightingHash = reader.ReadUInt64();
        var sourceWorldHash = reader.ReadUInt64();
        var volumeMin = ReadVector3(reader);
        var volumeMax = ReadVector3(reader);
        var raysPerProbe = reader.ReadUInt32();
        var bounceCount = reader.ReadUInt32();
        var randomSeed = reader.ReadUInt32();
        var maxSubdivisionLevel = reader.ReadUInt32();
        var minProbeSpacing = reader.ReadSingle();
        var normalBias = reader.ReadSingle();
        var viewBias = reader.ReadSingle();
        var maxRayDistance = reader.ReadSingle();
        var skyIndirectIntensity = reader.ReadSingle();
        var flags = reader.ReadUInt32();
        var brickCount = reader.ReadUInt32();
        var probeCount = reader.ReadUInt32();
        var invalidProbeCount = reader.ReadUInt32();
        var relocatedProbeCount = reader.ReadUInt32();
        var averageValidity = reader.ReadSingle();
        var bakeDurationSeconds = reader.ReadSingle();
        var stateName = ReadString(reader);
        var bakerVersion = ReadString(reader);
        var diagnostic = ReadString(reader);

        if (bakedStateCount != 1)
            throw new InvalidDataException("A .probes file must contain exactly one baked state.");
        if (payloadFlags != 0 || (flags & ~0x7u) != 0)
            throw new InvalidDataException("The .probes payload contains unsupported flags.");
        if (shOrder != SupportedSphericalHarmonicsOrder ||
            compression != SupportedCompression)
        {
            throw new InvalidDataException("The .probes representation is unsupported.");
        }
        if (brickCount is 0 or > MaxBrickCount ||
            probeCount is 0 or > MaxProbeCount)
        {
            throw new InvalidDataException("The .probes brick or probe count exceeds the supported limits.");
        }
        var expectedRecordBytes = checked(
            (ulong)brickCount * BrickRecordBytes +
            (ulong)probeCount * ProbeRecordBytes);
        if (expectedRecordBytes != checked((ulong)(stream.Length - stream.Position)))
            throw new InvalidDataException("The .probes record counts do not match the payload size.");
        if (!IsFinite(volumeMin) || !IsFinite(volumeMax) ||
            volumeMin.X >= volumeMax.X ||
            volumeMin.Y >= volumeMax.Y ||
            volumeMin.Z >= volumeMax.Z ||
            raysPerProbe == 0 ||
            raysPerProbe > ProbeVolumeBakeSettings.MaximumRaysPerProbe ||
            bounceCount == 0 ||
            bounceCount > ProbeVolumeBakeSettings.MaximumBounceCount ||
            maxSubdivisionLevel > ProbeVolumeBakeSettings.MaximumSubdivisionLevel ||
            !float.IsFinite(minProbeSpacing) || minProbeSpacing <= 0 ||
            !float.IsFinite(normalBias) || normalBias < 0 ||
            !float.IsFinite(viewBias) || viewBias < 0 ||
            !float.IsFinite(maxRayDistance) || maxRayDistance <= 0 ||
            !float.IsFinite(skyIndirectIntensity) || skyIndirectIntensity < 0 ||
            !float.IsFinite(averageValidity) || averageValidity is < 0 or > 1 ||
            !float.IsFinite(bakeDurationSeconds) || bakeDurationSeconds < 0)
        {
            throw new InvalidDataException("The .probes metadata contains invalid values.");
        }
        if (layoutHash == 0 || representationHash == 0 ||
            transportHash == 0 || lightingHash == 0 ||
            string.IsNullOrEmpty(stateName) ||
            string.IsNullOrEmpty(bakerVersion) ||
            invalidProbeCount > probeCount ||
            relocatedProbeCount > probeCount)
        {
            throw new InvalidDataException(
                "The .probes metadata is missing required identity or diagnostics.");
        }

        return new ProbeVolumeBinaryMetadata(
            formatVersion,
            bakedStateCount,
            shOrder,
            compression,
            layoutHash,
            representationHash,
            transportHash,
            lightingHash,
            sourceWorldHash,
            volumeMin,
            volumeMax,
            raysPerProbe,
            bounceCount,
            randomSeed,
            maxSubdivisionLevel,
            minProbeSpacing,
            normalBias,
            viewBias,
            maxRayDistance,
            skyIndirectIntensity,
            (flags & 1u) != 0,
            (flags & 2u) != 0,
            (flags & 4u) != 0,
            brickCount,
            probeCount,
            invalidProbeCount,
            relocatedProbeCount,
            averageValidity,
            bakeDurationSeconds,
            stateName,
            bakerVersion,
            diagnostic,
            payloadChecksum,
            stream.Length);
    }

    static ulong CalculateRemainingChecksum(Stream stream)
    {
        const ulong OffsetBasis = 1469598103934665603;
        const ulong Prime = 1099511628211;
        Span<byte> buffer = stackalloc byte[8192];
        ulong hash = OffsetBasis;
        int bytesRead;
        while ((bytesRead = stream.Read(buffer)) > 0)
        {
            foreach (var value in buffer[..bytesRead])
            {
                hash ^= value;
                hash *= Prime;
            }
        }
        return hash;
    }

    static uint ReadUInt32(ReadOnlySpan<byte> value, int offset) =>
        BinaryPrimitives.ReadUInt32LittleEndian(value[offset..]);

    static ulong ReadUInt64(ReadOnlySpan<byte> value, int offset) =>
        BinaryPrimitives.ReadUInt64LittleEndian(value[offset..]);

    static Vector3 ReadVector3(BinaryReader reader) => new(
        reader.ReadSingle(),
        reader.ReadSingle(),
        reader.ReadSingle());

    static bool IsFinite(Vector3 value) =>
        float.IsFinite(value.X) &&
        float.IsFinite(value.Y) &&
        float.IsFinite(value.Z);

    static string ReadString(BinaryReader reader)
    {
        var length = reader.ReadUInt32();
        if (length > MaxMetadataStringBytes ||
            length > reader.BaseStream.Length - reader.BaseStream.Position)
        {
            throw new InvalidDataException("A .probes metadata string is truncated or too large.");
        }
        var bytes = reader.ReadBytes(checked((int)length));
        if (bytes.Length != length)
            throw new EndOfStreamException("A .probes metadata string is truncated.");
        return Encoding.UTF8.GetString(bytes);
    }
}
