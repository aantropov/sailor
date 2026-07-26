using SailorEditor.Content;

namespace Editor.Tests;

public sealed class AssetSourcePathContractTests : IDisposable
{
    readonly string root = Path.Combine(
        Path.GetTempPath(),
        "sailor-asset-source-contract",
        Guid.NewGuid().ToString("N"));

    public AssetSourcePathContractTests()
    {
        Directory.CreateDirectory(root);
    }

    [Fact]
    public void ResolvesTheDeclaredSourceBesidePrimaryMetadata()
    {
        var source = WriteSource("ComputeParticles.shader");
        var metadata = source + ".asset";

        Assert.True(AssetSourcePathContract.TryResolve(
            metadata,
            "ComputeParticles.shader",
            out var resolution,
            out var error), error);
        Assert.Equal(source, resolution.SourcePath);
        Assert.True(resolution.OwnsSourceFile);
        Assert.Equal(".shader", resolution.AssetExtension);
    }

    [Fact]
    public void ResolvesSecondaryMetadataWithoutOwningTheSharedSource()
    {
        var source = WriteSource("Duck.glb");
        var metadata = Path.Combine(root, "Duck_BaseColor.png.asset");

        Assert.True(AssetSourcePathContract.TryResolve(
            metadata,
            "Duck.glb",
            out var resolution,
            out var error), error);
        Assert.Equal(source, resolution.SourcePath);
        Assert.False(resolution.OwnsSourceFile);
        Assert.Equal(".png", resolution.AssetExtension);
    }

    [Fact]
    public void ResolvesGeneratedAnimationTypeFromSecondaryMetadataName()
    {
        var source = WriteSource("Character.glb");
        var metadata = Path.Combine(root, "Character_Walk.anim.asset");

        Assert.True(AssetSourcePathContract.TryResolve(
            metadata,
            "Character.glb",
            out var resolution,
            out var error), error);
        Assert.False(resolution.OwnsSourceFile);
        Assert.Equal(".anim", resolution.AssetExtension);
    }

    [Theory]
    [InlineData("../Outside.shader")]
    [InlineData("Nested/Outside.shader")]
    [InlineData("Nested\\Outside.shader")]
    [InlineData("/tmp/Outside.shader")]
    public void RejectsSourcesOutsideTheMetadataDirectory(string filename)
    {
        var metadata = Path.Combine(root, "Unsafe.asset");

        Assert.False(AssetSourcePathContract.TryResolve(
            metadata,
            filename,
            out _,
            out var error));
        Assert.NotEmpty(error);
    }

    [Fact]
    public void RejectsAMissingDeclaredSource()
    {
        var metadata = Path.Combine(root, "Missing.shader.asset");

        Assert.False(AssetSourcePathContract.TryResolve(
            metadata,
            "Missing.shader",
            out _,
            out var error));
        Assert.Contains("does not exist", error, StringComparison.Ordinal);
    }

    string WriteSource(string filename)
    {
        var path = Path.Combine(root, filename);
        File.WriteAllText(path, "source");
        return Path.GetFullPath(path);
    }

    public void Dispose()
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
    }
}
