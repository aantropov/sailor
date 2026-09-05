using SailorEditor.Content;
using SailorEditor.Services;

namespace SailorEditor.Tests;

public sealed class GIProbesBakeOutputPolicyTests
{
    [Fact]
    public void AvailableStateName_SkipsBoundStatesAndExistingOutputs()
    {
        var existingOutputs = new HashSet<string>(StringComparer.Ordinal)
        {
            "Day2"
        };

        var stateName = GIProbesBakeOutputPolicy.FindAvailableStateName(
            "Day",
            ["Day"],
            existingOutputs.Contains);

        Assert.Equal("Day3", stateName);
    }

    [Fact]
    public void ResolveWriteTarget_RejectsExistingOutputBeforeWorldSave()
    {
        using var content = TemporaryContentRoot.Create();
        var outputPath = Path.Combine(content.Path, "Lighting", "Day.probes");
        Directory.CreateDirectory(Path.GetDirectoryName(outputPath)!);
        File.WriteAllText(outputPath, "existing bake");

        var resolved = GIProbesBakeOutputPolicy.TryResolveWriteTarget(
            content.Path,
            "Lighting/Day.probes",
            overwrite: false,
            out var physicalPath,
            out var error);

        Assert.False(resolved);
        Assert.Empty(physicalPath);
        Assert.Contains("already exists", error, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("Overwrite existing file", error, StringComparison.Ordinal);
    }

    [Fact]
    public void ResolveWriteTarget_AllowsExplicitOverwrite()
    {
        using var content = TemporaryContentRoot.Create();
        var outputPath = Path.Combine(content.Path, "Lighting", "Day.probes");
        Directory.CreateDirectory(Path.GetDirectoryName(outputPath)!);
        File.WriteAllText(outputPath, "existing bake");

        var resolved = GIProbesBakeOutputPolicy.TryResolveWriteTarget(
            content.Path,
            "Lighting/Day.probes",
            overwrite: true,
            out var physicalPath,
            out var error);

        Assert.True(resolved, error);
        Assert.True(ProjectContentPathPolicy.IsSamePath(outputPath, physicalPath));
    }

    [Theory]
    [InlineData("../Outside.probes")]
    [InlineData("Lighting/Day.asset")]
    public void ResolveWriteTarget_RejectsUnsafeOrWrongExtension(string output)
    {
        using var content = TemporaryContentRoot.Create();

        var resolved = GIProbesBakeOutputPolicy.TryResolveWriteTarget(
            content.Path,
            output,
            overwrite: false,
            out var physicalPath,
            out var error);

        Assert.False(resolved);
        Assert.Empty(physicalPath);
        Assert.False(string.IsNullOrWhiteSpace(error));
    }

    sealed class TemporaryContentRoot : IDisposable
    {
        TemporaryContentRoot(string path) => Path = path;

        public string Path { get; }

        public static TemporaryContentRoot Create()
        {
            var path = System.IO.Path.Combine(
                System.IO.Path.GetTempPath(),
                "SailorProbeBakeOutputPolicyTests",
                Guid.NewGuid().ToString("N"),
                "Content");
            Directory.CreateDirectory(path);
            return new TemporaryContentRoot(path);
        }

        public void Dispose()
        {
            if (Directory.Exists(Path))
                Directory.Delete(Path, recursive: true);
        }
    }
}
