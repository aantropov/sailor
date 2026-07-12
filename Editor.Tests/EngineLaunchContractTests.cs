using SailorEditor.Services;

public class EngineLaunchContractTests
{
    [Fact]
    public void Resolve_UsesActiveWorkspaceRoot()
    {
        var fallbackRoot = Path.Combine(Path.GetTempPath(), "Sailor");
        var workspaceRoot = Path.Combine(Path.GetTempPath(), "Sailor Workspaces", "Sandbox");
        var manifestPath = Path.Combine(workspaceRoot, "Sandbox Project.sailor");

        var context = EngineLaunchContract.Resolve(workspaceRoot, manifestPath, fallbackRoot);

        Assert.Equal(Path.GetFullPath(workspaceRoot), context.WorkspaceRoot);
        Assert.Equal(Path.GetFullPath(manifestPath), context.WorkspaceManifestPath);
        Assert.Equal(Path.Combine(workspaceRoot, "Content"), context.ContentDirectory);
        Assert.Equal(Path.Combine(workspaceRoot, "Cache"), context.CacheDirectory);
        Assert.Equal(Path.Combine(workspaceRoot, "Cache", "Temp.world"), context.TempWorldFilePath);
        Assert.Equal(Path.Combine(workspaceRoot, "Cache", "EditorTypes.yaml"), context.EditorTypesCacheFilePath);
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData("   ")]
    public void Resolve_UsesFallbackWithoutActiveWorkspace(string? activeWorkspaceRoot)
    {
        var fallbackRoot = Path.Combine(Path.GetTempPath(), "Sailor");

        var context = EngineLaunchContract.Resolve(activeWorkspaceRoot, fallbackRoot);

        Assert.Equal(Path.GetFullPath(fallbackRoot), context.WorkspaceRoot);
    }

    [Fact]
    public void BuildArguments_PreservesPathsWithSpacesAsSingleTokens()
    {
        var workspaceRoot = Path.Combine(Path.GetTempPath(), "Sailor Workspaces", "Sandbox");
        var manifestPath = Path.Combine(workspaceRoot, "Sandbox Project.sailor");
        var context = EngineLaunchContract.Resolve(workspaceRoot, manifestPath, Path.GetTempPath());

        var arguments = context.BuildArguments("../Cache/Temp world.world", ["--noconsole", "--editor"]);

        Assert.Equal(
            [
                "--workspace",
                Path.GetFullPath(workspaceRoot),
                "--workspace-manifest",
                Path.GetFullPath(manifestPath),
                "--world",
                "../Cache/Temp world.world",
                "--noconsole",
                "--editor"
            ],
            arguments);
    }

    [Fact]
    public void BuildInteropArguments_KeepsWorldFlagAndValueSeparate()
    {
        var context = EngineLaunchContract.Resolve(Path.Combine(Path.GetTempPath(), "Sandbox"), Path.GetTempPath());

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

        var context = EngineLaunchContract.Resolve(null, ignoredManifest, fallbackRoot);

        Assert.Null(context.WorkspaceManifestPath);
        Assert.DoesNotContain("--workspace-manifest", context.BuildArguments("Editor.world"));
    }
}
