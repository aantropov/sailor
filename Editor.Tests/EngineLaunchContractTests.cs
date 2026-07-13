using SailorEditor.Services;

public class EngineLaunchContractTests
{
    [Fact]
    public void Resolve_UsesResolvedActiveWorkspacePaths()
    {
        var fallbackRoot = Path.Combine(Path.GetTempPath(), "Sailor");
        var workspaceRoot = Path.Combine(Path.GetTempPath(), "Sailor Workspaces", "Sandbox");
        var manifestPath = Path.Combine(workspaceRoot, "Sandbox Project.sailor");
        var contentDirectory = Path.Combine(workspaceRoot, "Project Content");
        var cacheDirectory = Path.Combine(workspaceRoot, "Generated Cache");

        var context = Resolve(
            workspaceRoot,
            manifestPath,
            contentDirectory,
            cacheDirectory,
            fallbackRoot);

        Assert.Equal(Path.GetFullPath(workspaceRoot), context.WorkspaceRoot);
        Assert.Equal(Path.GetFullPath(manifestPath), context.WorkspaceManifestPath);
        Assert.Equal(Path.GetFullPath(contentDirectory), context.ContentDirectory);
        Assert.Equal(Path.GetFullPath(cacheDirectory), context.CacheDirectory);
        Assert.Equal(Path.Combine(cacheDirectory, "Temp.world"), context.TempWorldFilePath);
        Assert.Equal(
            Path.GetRelativePath(contentDirectory, Path.Combine(cacheDirectory, "Temp.world")),
            context.TempWorldRuntimePath);
        Assert.Equal(Path.Combine(cacheDirectory, "EditorTypes.yaml"), context.EditorTypesCacheFilePath);
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("   ")]
    public void Resolve_UsesFallbackWithoutActiveWorkspace(string? activeWorkspaceRoot)
    {
        var fallbackRoot = Path.Combine(Path.GetTempPath(), "Sailor");

        var context = EngineLaunchContract.Resolve(
            activeWorkspaceRoot,
            null,
            null,
            null,
            fallbackRoot);

        Assert.Equal(Path.GetFullPath(fallbackRoot), context.WorkspaceRoot);
        Assert.Equal(Path.Combine(Path.GetFullPath(fallbackRoot), "Content"), context.ContentDirectory);
        Assert.Equal(Path.Combine(Path.GetFullPath(fallbackRoot), "Cache"), context.CacheDirectory);
        Assert.Equal(Path.Combine(Path.GetFullPath(fallbackRoot), "Cache", "EditorTypes.yaml"), context.EditorTypesCacheFilePath);
    }

    [Fact]
    public void BuildArguments_PreservesPathsWithSpacesAsSingleTokens()
    {
        var workspaceRoot = Path.Combine(Path.GetTempPath(), "Sailor Workspaces", "Sandbox");
        var manifestPath = Path.Combine(workspaceRoot, "Sandbox Project.sailor");
        var contentDirectory = Path.Combine(workspaceRoot, "Project Content");
        var cacheDirectory = Path.Combine(workspaceRoot, "Cache With Spaces");
        var context = Resolve(
            workspaceRoot,
            manifestPath,
            contentDirectory,
            cacheDirectory,
            Path.GetTempPath());

        var arguments = context.BuildArguments(context.TempWorldRuntimePath, ["--noconsole", "--editor"]);

        Assert.Equal(
            [
                "--workspace",
                Path.GetFullPath(workspaceRoot),
                "--workspace-manifest",
                Path.GetFullPath(manifestPath),
                "--world",
                Path.GetRelativePath(contentDirectory, Path.Combine(cacheDirectory, "Temp.world")),
                "--noconsole",
                "--editor"
            ],
            arguments);
    }

    [Fact]
    public void BuildInteropArguments_KeepsWorldFlagAndValueSeparate()
    {
        var workspaceRoot = Path.Combine(Path.GetTempPath(), "Sandbox");
        var context = Resolve(
            workspaceRoot,
            null,
            Path.Combine(workspaceRoot, "Content"),
            Path.Combine(workspaceRoot, "Cache"),
            Path.GetTempPath());

        var arguments = context.BuildInteropArguments("SailorEditor", "Editor.world", ["--editor"]);

        Assert.Equal("SailorEditor", arguments[0]);
        Assert.Equal("--workspace", arguments[1]);
        Assert.Equal("--world", arguments[3]);
        Assert.Equal("Editor.world", arguments[4]);
        Assert.DoesNotContain("--world Editor.world", arguments);
    }

    [Fact]
    public void Resolve_DoesNotAttachManifestWithoutActiveWorkspace()
    {
        var fallbackRoot = Path.Combine(Path.GetTempPath(), "Sailor");
        var ignoredManifest = Path.Combine(Path.GetTempPath(), "Ignored.sailor");

        var context = Resolve(null, ignoredManifest, null, null, fallbackRoot);

        Assert.Null(context.WorkspaceManifestPath);
        Assert.DoesNotContain("--workspace-manifest", context.BuildArguments("Editor.world"));
    }

    [Theory]
    [InlineData(null, "Cache")]
    [InlineData("Content", null)]
    public void Resolve_RejectsActiveWorkspaceWithoutResolvedPaths(
        string? contentDirectoryName,
        string? cacheDirectoryName)
    {
        var workspaceRoot = Path.Combine(Path.GetTempPath(), "Sandbox");
        var contentDirectory = contentDirectoryName is null
            ? null
            : Path.Combine(workspaceRoot, contentDirectoryName);
        var cacheDirectory = cacheDirectoryName is null
            ? null
            : Path.Combine(workspaceRoot, cacheDirectoryName);

        Assert.Throws<ArgumentException>(() => Resolve(
            workspaceRoot,
            null,
            contentDirectory,
            cacheDirectory,
            Path.GetTempPath()));
    }

    static EngineLaunchContext Resolve(
        string? workspaceRoot,
        string? manifestPath,
        string? contentDirectory,
        string? cacheDirectory,
        string fallbackRoot)
        => EngineLaunchContract.Resolve(
            workspaceRoot,
            manifestPath,
            contentDirectory,
            cacheDirectory,
            fallbackRoot);
}
