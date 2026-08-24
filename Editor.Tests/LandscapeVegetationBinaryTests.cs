using System.Buffers.Binary;
using SailorEditor.Mcp;

namespace SailorEditor.Tests;

public sealed class LandscapeVegetationBinaryTests
{
    [Fact]
    public void ReadPageReturnsExactFormatIdentityMatrixAndInstanceSettings()
    {
        var filepath = Path.Combine(
            Path.GetTempPath(),
            $"sailor-landscape-{Guid.NewGuid():N}.vegetation");
        try
        {
            var bytes = new byte[
                LandscapeVegetationBinary.HeaderSize +
                LandscapeVegetationBinary.ChunkRecordSize +
                LandscapeVegetationBinary.InstanceRecordSize];
            "SLVEG001"u8.CopyTo(bytes);
            U32(bytes, 8, LandscapeVegetationBinary.Version);
            U32(bytes, 12, LandscapeVegetationBinary.EndianMarker);
            U32(bytes, 16, LandscapeVegetationBinary.HeaderSize);
            U32(bytes, 20, LandscapeVegetationBinary.ChunkRecordSize);
            U32(bytes, 24, LandscapeVegetationBinary.InstanceRecordSize);
            U32(bytes, 28, 1);
            U32(bytes, 32, 1);
            U32(bytes, 36, 2);
            U32(bytes, 40, 1);
            U64(bytes, 44, 1);
            F32(bytes, 52, 64.0f);
            U32(bytes, 56, 1);

            var chunkOffset = checked((int)LandscapeVegetationBinary.HeaderSize);
            U32(bytes, chunkOffset, 0);
            U32(bytes, chunkOffset + 4, 0);
            U64(bytes, chunkOffset + 8, 0);
            U32(bytes, chunkOffset + 16, 1);

            var instanceOffset = checked(chunkOffset +
                (int)LandscapeVegetationBinary.ChunkRecordSize);
            U64(bytes, instanceOffset, 0x1122334455667788UL);
            U32(bytes, instanceOffset + 8, 1);
            U32(bytes, instanceOffset + 12, LandscapeVegetationBinary.EnabledFlag);
            float[] expectedMatrix =
            [
                1, 0, 0, 0,
                0, 2, 0, 0,
                0, 0, 3, 0,
                4, 5, 6, 1
            ];
            for (var matrixIndex = 0; matrixIndex < expectedMatrix.Length; ++matrixIndex)
            {
                F32(bytes, instanceOffset + 16 + matrixIndex * sizeof(float),
                    expectedMatrix[matrixIndex]);
            }
            I32(bytes, instanceOffset + 80, -2);
            F32(bytes, instanceOffset + 84, 0.75f);
            F32(bytes, instanceOffset + 88, 1.5f);
            File.WriteAllBytes(filepath, bytes);

            var result = LandscapeVegetationBinary.ReadPage(
                filepath,
                "VEGETATION-FILE-ID",
                "Landscape/Vegetation/Landscape.vegetation",
                requestedOffset: 0,
                requestedLimit: 32);

            Assert.True(result.Succeeded, result.Message);
            Assert.Equal("VEGETATION-FILE-ID", result.VegetationFileId);
            Assert.Equal("SLVEG001", result.Format.Magic);
            Assert.Equal((ulong)1, result.TotalInstanceCount);
            var instance = Assert.Single(result.Instances);
            Assert.Equal("0x1122334455667788", instance.StableId);
            Assert.Equal((uint)1, instance.ProfileIndex);
            Assert.True(instance.Enabled);
            Assert.Equal(expectedMatrix, instance.Matrix);
            Assert.Equal(-2, instance.LodBias);
            Assert.Equal(0.75f, instance.CullDistanceScale);
            Assert.Equal(1.5f, instance.ShadowDistanceScale);
        }
        finally
        {
            File.Delete(filepath);
        }
    }

    static void U32(byte[] bytes, int offset, uint value) =>
        BinaryPrimitives.WriteUInt32LittleEndian(
            bytes.AsSpan(offset, sizeof(uint)), value);

    static void I32(byte[] bytes, int offset, int value) =>
        BinaryPrimitives.WriteInt32LittleEndian(
            bytes.AsSpan(offset, sizeof(int)), value);

    static void U64(byte[] bytes, int offset, ulong value) =>
        BinaryPrimitives.WriteUInt64LittleEndian(
            bytes.AsSpan(offset, sizeof(ulong)), value);

    static void F32(byte[] bytes, int offset, float value) =>
        I32(bytes, offset, BitConverter.SingleToInt32Bits(value));
}
