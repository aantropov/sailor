using SailorEditor.ViewModels;
using System.Text;

namespace SailorEditor.Tests;

public sealed class GIProbesBinaryMetadataTests
{
    [Fact]
    public void ReadsOneStateMetadataAndValidatesChecksum()
    {
        using var asset = TestProbeAsset.Create();

        var metadata = GIProbesBinaryMetadata.Read(asset.File);

        Assert.Equal(1u, metadata.BakedStateCount);
        Assert.Equal("Evening", metadata.StateName);
        Assert.Equal("Test Baker", metadata.BakerVersion);
        Assert.Equal(17u, metadata.RaysPerProbe);
        Assert.Equal(0.05f, metadata.NormalBias);
        Assert.Equal(0.07f, metadata.ViewBias);
        Assert.Equal(1.25f, metadata.SkyIndirectIntensity);
        Assert.Equal(0x1111222233334444UL, metadata.LayoutHash);
        Assert.Equal(0x5555666677778888UL, metadata.RepresentationHash);
        Assert.Equal(1u, metadata.BrickCount);
        Assert.Equal(1u, metadata.ProbeCount);
        Assert.True(metadata.IncludeSky);
        Assert.True(metadata.IncludeDirectLighting);
        Assert.False(metadata.IncludeEmissive);
    }

    [Fact]
    public void RejectsPayloadCorruption()
    {
        using var asset = TestProbeAsset.Create();
        var bytes = File.ReadAllBytes(asset.File.FullName);
        bytes[^1] ^= 0x40;
        File.WriteAllBytes(asset.File.FullName, bytes);

        var exception = Assert.Throws<InvalidDataException>(
            () => GIProbesBinaryMetadata.Read(asset.File));

        Assert.Contains("checksum", exception.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void RejectsMoreThanOneBakedState()
    {
        using var asset = TestProbeAsset.Create(bakedStateCount: 2);

        var exception = Assert.Throws<InvalidDataException>(
            () => GIProbesBinaryMetadata.Read(asset.File));

        Assert.Contains("exactly one", exception.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void RejectsRecordCountThatDoesNotMatchPayload()
    {
        using var asset = TestProbeAsset.Create(probeCount: 2);

        var exception = Assert.Throws<InvalidDataException>(
            () => GIProbesBinaryMetadata.Read(asset.File));

        Assert.Contains("record counts", exception.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void RejectsMissingStateIdentity()
    {
        using var asset = TestProbeAsset.Create(stateName: string.Empty);

        var exception = Assert.Throws<InvalidDataException>(
            () => GIProbesBinaryMetadata.Read(asset.File));

        Assert.Contains("identity", exception.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void RejectsUnknownHeaderFlags()
    {
        using var asset = TestProbeAsset.Create(headerFlags: 1);

        var exception = Assert.Throws<InvalidDataException>(
            () => GIProbesBinaryMetadata.Read(asset.File));

        Assert.Contains("flags", exception.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void RejectsUnknownPayloadFlags()
    {
        using var asset = TestProbeAsset.Create(payloadFlags: 1);

        var exception = Assert.Throws<InvalidDataException>(
            () => GIProbesBinaryMetadata.Read(asset.File));

        Assert.Contains("flags", exception.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void RejectsUnsupportedSamplingLimits()
    {
        using var asset = TestProbeAsset.Create(raysPerProbe: 65537);

        var exception = Assert.Throws<InvalidDataException>(
            () => GIProbesBinaryMetadata.Read(asset.File));

        Assert.Contains("invalid values", exception.Message, StringComparison.OrdinalIgnoreCase);
    }

    sealed class TestProbeAsset : IDisposable
    {
        TestProbeAsset(string path) => File = new FileInfo(path);

        public FileInfo File { get; }

        public static TestProbeAsset Create(
            uint bakedStateCount = 1,
            uint probeCount = 1,
            string stateName = "Evening",
            uint headerFlags = 0,
            uint payloadFlags = 0,
            uint raysPerProbe = 17)
        {
            using var payloadStream = new MemoryStream();
            using (var payload = new BinaryWriter(
                payloadStream,
                Encoding.UTF8,
                leaveOpen: true))
            {
                payload.Write(bakedStateCount);
                payload.Write(2u);
                payload.Write(0u);
                payload.Write(payloadFlags);
                payload.Write(0x1111222233334444UL);
                payload.Write(0x5555666677778888UL);
                payload.Write(0x0123456789abcdefUL);
                payload.Write(0xfedcba9876543210UL);
                payload.Write(0xaabbccddeeff0011UL);
                WriteVector3(payload, -1.0f, -2.0f, -3.0f);
                WriteVector3(payload, 4.0f, 5.0f, 6.0f);
                payload.Write(raysPerProbe);
                payload.Write(3u);
                payload.Write(1729u);
                payload.Write(2u);
                payload.Write(0.75f);
                payload.Write(0.05f);
                payload.Write(0.07f);
                payload.Write(250.0f);
                payload.Write(1.25f);
                payload.Write(5u);
                payload.Write(1u);
                payload.Write(probeCount);
                payload.Write(0u);
                payload.Write(1u);
                payload.Write(0.875f);
                payload.Write(1.25f);
                WriteString(payload, stateName);
                WriteString(payload, "Test Baker");
                WriteString(payload, "All good");
                payload.Write(new byte[48 + 212]);
            }

            var payloadBytes = payloadStream.ToArray();
            var path = Path.Combine(
                Path.GetTempPath(),
                $"sailor-probes-metadata-{Guid.NewGuid():N}.probes");
            using (var stream = new FileStream(path, FileMode.CreateNew, FileAccess.Write))
            using (var writer = new BinaryWriter(stream, Encoding.UTF8, leaveOpen: true))
            {
                writer.Write("SLRPROBE"u8);
                writer.Write(1u);
                writer.Write(0x01020304u);
                writer.Write(40u);
                writer.Write(headerFlags);
                writer.Write((ulong)payloadBytes.Length);
                writer.Write(Checksum(payloadBytes));
                writer.Write(payloadBytes);
            }
            return new TestProbeAsset(path);
        }

        public void Dispose()
        {
            try
            {
                File.Delete();
            }
            catch
            {
            }
        }

        static void WriteVector3(
            BinaryWriter writer,
            float x,
            float y,
            float z)
        {
            writer.Write(x);
            writer.Write(y);
            writer.Write(z);
        }

        static void WriteString(BinaryWriter writer, string value)
        {
            var bytes = Encoding.UTF8.GetBytes(value);
            writer.Write((uint)bytes.Length);
            writer.Write(bytes);
        }

        static ulong Checksum(ReadOnlySpan<byte> bytes)
        {
            ulong hash = 14695981039346656037;
            foreach (var value in bytes)
            {
                hash ^= value;
                hash *= 1099511628211;
            }
            return hash;
        }
    }
}
