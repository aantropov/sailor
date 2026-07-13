using SailorEditor.Services;
using SailorEditor.Workspace;

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
        Assert.Equal("workspace-sandbox", context.WorkspaceIdentity);
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
        Assert.StartsWith("legacy-root:", context.WorkspaceIdentity, StringComparison.Ordinal);
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

    [Fact]
    public void WorkspaceIdentity_ParticipatesInExactLaunchContextEquality()
    {
        var workspaceRoot = Path.Combine(Path.GetTempPath(), "Sandbox");
        var first = EngineLaunchContract.Resolve(
            workspaceRoot,
            null,
            Path.Combine(workspaceRoot, "Content"),
            Path.Combine(workspaceRoot, "Cache"),
            Path.GetTempPath(),
            "workspace-a");
        var second = first with { WorkspaceIdentity = "workspace-b" };

        Assert.NotEqual(first, second);
    }

    [Fact]
    public void Resolve_PreservesUnicodeManifestWorkspaceIdentity()
    {
        var workspaceRoot = Path.Combine(Path.GetTempPath(), "Sandbox");
        var context = EngineLaunchContract.Resolve(
            workspaceRoot,
            null,
            Path.Combine(workspaceRoot, "Content"),
            Path.Combine(workspaceRoot, "Cache"),
            Path.GetTempPath(),
            "  рабочая-область-а  ");

        Assert.Equal("рабочая-область-а", context.WorkspaceIdentity);
    }

    [Fact]
    public void Resolve_LegacyIdentityUsesThePhysicalWorkspaceRoot()
    {
        var fixtureRoot = Path.Combine(
            Path.GetTempPath(),
            $"SailorEngineLaunchIdentity-{Guid.NewGuid():N}");
        var physicalRoot = Path.Combine(fixtureRoot, "Physical-РАБОЧАЯ-AZ");
        var selectedRoot = physicalRoot;
        Directory.CreateDirectory(physicalRoot);

        try
        {
            if (!OperatingSystem.IsWindows())
            {
                selectedRoot = Path.Combine(fixtureRoot, "Alias");
                Directory.CreateSymbolicLink(selectedRoot, physicalRoot);
            }

            var context = EngineLaunchContract.Resolve(
                null,
                null,
                null,
                null,
                selectedRoot);
            var expectedRoot = WorkspacePathPolicy.NormalizePhysicalPath(physicalRoot)
                .Replace(Path.DirectorySeparatorChar, '/');
            if (OperatingSystem.IsWindows())
                expectedRoot = FoldAsciiCase(expectedRoot);

            Assert.Equal($"legacy-root:{expectedRoot}", context.WorkspaceIdentity);
            Assert.Contains("РАБОЧАЯ", context.WorkspaceIdentity, StringComparison.Ordinal);
        }
        finally
        {
            Directory.Delete(fixtureRoot, recursive: true);
        }
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
            fallbackRoot,
            workspaceRoot is null ? null : "workspace-sandbox");

    static string FoldAsciiCase(string value)
    {
        var characters = value.ToCharArray();
        for (var index = 0; index < characters.Length; ++index)
        {
            if (characters[index] is >= 'A' and <= 'Z')
                characters[index] = (char)(characters[index] + ('a' - 'A'));
        }

        return new string(characters);
    }
}
