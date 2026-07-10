using SailorEditor.Services;

public class EngineLaunchContractTests
{
    [Fact]
    public void Resolve_UsesActiveWorkspaceRoot()
    {
        var fallbackRoot = Path.Combine(Path.GetTempPath(), "Sailor");
        var workspaceRoot = Path.Combine(Path.GetTempPath(), "Sailor Workspaces", "Sandbox");

        var context = EngineLaunchContract.Resolve(workspaceRoot, fallbackRoot);

        Assert.Equal(Path.GetFullPath(workspaceRoot), context.WorkspaceRoot);
        Assert.Equal(Path.Combine(workspaceRoot, "Content"), context.ContentDirectory);
        Assert.Equal(Path.Combine(workspaceRoot, "Cache"), context.CacheDirectory);
        Assert.Equal(Path.Combine(workspaceRoot, "Cache", "Temp.world"), context.TempWorldFilePath);
        Assert.Equal(Path.Combine(workspaceRoot, "Cache", "EngineTypes.yaml"), context.EngineTypesCacheFilePath);
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
        var context = EngineLaunchContract.Resolve(workspaceRoot, Path.GetTempPath());

        var arguments = context.BuildArguments("../Cache/Temp world.world", ["--noconsole", "--editor"]);

        Assert.Equal(
            ["--workspace", Path.GetFullPath(workspaceRoot), "--world", "../Cache/Temp world.world", "--noconsole", "--editor"],
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
}
