using SailorEditor.Workspace;

namespace SailorEditor.Editor.Tests;

public sealed class WorkspaceManifestTests
{
    [Fact]
    public void CreateDefault_CreatesValidManifest()
    {
        var manifest = WorkspaceManifest.CreateDefault("Sandbox", "../Sailor", "workspace-id");
        var serializer = new WorkspaceManifestSerializer();

        var validation = serializer.Validate(manifest);

        Assert.True(validation.IsValid);
        Assert.Equal(WorkspaceManifest.CurrentVersion, manifest.ManifestVersion);
        Assert.Equal("workspace-id", manifest.WorkspaceId);
        Assert.Equal("Sandbox", manifest.Name);
        Assert.Equal("../Sailor", manifest.EnginePath);
        Assert.Equal("Content", manifest.ContentPath);
        Assert.Equal("Source", manifest.SourcePath);
        Assert.Equal("Generated", manifest.GeneratedProjectPath);
        Assert.Equal("Cache", manifest.CachePath);
    }

    [Fact]
    public async Task SaveAndLoad_RoundTripsYamlManifest()
    {
        var path = Path.Combine(Path.GetTempPath(), $"sailor-workspace-{Guid.NewGuid():N}.sailor");
        var serializer = new WorkspaceManifestSerializer();
        var manifest = WorkspaceManifest.CreateDefault("Sandbox", "../Sailor", "workspace-id") with
        {
            ContentPath = "./Game\\Content\\",
            SourcePath = "Game\\Source",
            GeneratedProjectPath = "./Generated\\Project\\",
            CachePath = "Cache\\"
        };

        try
        {
            await serializer.SaveAsync(path, manifest);

            var result = await serializer.LoadAsync(path);

            Assert.True(result.Succeeded);
            Assert.NotNull(result.Manifest);
            Assert.Equal("Game/Content", result.Manifest.ContentPath);
            Assert.Equal("Game/Source", result.Manifest.SourcePath);
            Assert.Equal("Generated/Project", result.Manifest.GeneratedProjectPath);
            Assert.Equal("Cache", result.Manifest.CachePath);
        }
        finally
        {
            if (File.Exists(path))
                File.Delete(path);
        }
    }

    [Fact]
    public async Task SaveAsync_RejectsInvalidManifest()
    {
        var path = Path.Combine(Path.GetTempPath(), $"sailor-workspace-{Guid.NewGuid():N}.sailor");
        var serializer = new WorkspaceManifestSerializer();
        var manifest = new WorkspaceManifest();

        await Assert.ThrowsAsync<InvalidOperationException>(() => serializer.SaveAsync(path, manifest));
        Assert.False(File.Exists(path));
    }

    [Fact]
    public void Deserialize_ReportsMissingRequiredFields()
    {
        var serializer = new WorkspaceManifestSerializer();

        var result = serializer.Deserialize("""
manifestVersion: 1
name: ""
contentPath: Content
""");

        Assert.False(result.Succeeded);
        Assert.NotNull(result.Manifest);
        Assert.Contains(result.Validation.Issues, x => x.Field == nameof(WorkspaceManifest.WorkspaceId));
        Assert.Contains(result.Validation.Issues, x => x.Field == nameof(WorkspaceManifest.Name));
        Assert.Contains(result.Validation.Issues, x => x.Field == nameof(WorkspaceManifest.EnginePath));
        Assert.Contains(result.Validation.Issues, x => x.Field == nameof(WorkspaceManifest.SourcePath));
        Assert.Contains(result.Validation.Issues, x => x.Field == nameof(WorkspaceManifest.GeneratedProjectPath));
        Assert.Contains(result.Validation.Issues, x => x.Field == nameof(WorkspaceManifest.CachePath));
    }

    [Fact]
    public void Deserialize_ReportsUnsupportedVersion()
    {
        var serializer = new WorkspaceManifestSerializer();

        var result = serializer.Deserialize("""
manifestVersion: 999
workspaceId: workspace-id
name: Sandbox
enginePath: ../Sailor
contentPath: Content
sourcePath: Source
generatedProjectPath: Generated
cachePath: Cache
""");

        Assert.False(result.Succeeded);
        Assert.Contains(result.Validation.Issues, x =>
            x.Field == nameof(WorkspaceManifest.ManifestVersion) &&
            x.Message.Contains("Unsupported workspace manifest version", StringComparison.Ordinal));
    }

    [Fact]
    public void Deserialize_ReportsMissingManifestVersion()
    {
        var serializer = new WorkspaceManifestSerializer();

        var result = serializer.Deserialize("""
workspaceId: workspace-id
name: Sandbox
enginePath: ../Sailor
contentPath: Content
sourcePath: Source
generatedProjectPath: Generated
cachePath: Cache
""");

        Assert.False(result.Succeeded);
        Assert.Contains(result.Validation.Issues, x => x.Field == nameof(WorkspaceManifest.ManifestVersion));
    }

    [Fact]
    public void Deserialize_NormalizesNullStringFieldsBeforeValidation()
    {
        var serializer = new WorkspaceManifestSerializer();

        var result = serializer.Deserialize("""
manifestVersion: 1
workspaceId:
name:
enginePath:
contentPath:
sourcePath:
generatedProjectPath:
cachePath:
""");

        Assert.False(result.Succeeded);
        Assert.NotNull(result.Manifest);
        Assert.Equal(string.Empty, result.Manifest.WorkspaceId);
        Assert.Contains(result.Validation.Issues, x => x.Field == nameof(WorkspaceManifest.WorkspaceId));
    }

    [Theory]
    [InlineData("./Content", "Content")]
    [InlineData(".\\Content\\Textures\\", "Content/Textures")]
    [InlineData("Generated\\Project", "Generated/Project")]
    [InlineData("  Cache/  ", "Cache")]
    public void NormalizePath_UsesStableManifestFormat(string input, string expected)
    {
        Assert.Equal(expected, WorkspaceManifestPaths.Normalize(input));
    }
}
