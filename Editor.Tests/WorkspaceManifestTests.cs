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
        Assert.Equal(WorkspaceEngineReferenceKinds.Source, manifest.EngineReferenceKind);
        Assert.Equal("Cache/Build", manifest.BuildPath);
        Assert.Equal("Binaries", manifest.LogicOutputPath);
        Assert.Equal("SailorGame", manifest.LogicModuleName);
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
            CachePath = "Cache\\",
            EngineReferenceKind = " INSTALLED ",
            BuildPath = ".\\Cache\\Build Output\\",
            LogicOutputPath = ".\\Game Binaries\\",
            LogicModuleName = " SandboxLogic "
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
            Assert.Equal(WorkspaceEngineReferenceKinds.Installed, result.Manifest.EngineReferenceKind);
            Assert.Equal("Cache/Build Output", result.Manifest.BuildPath);
            Assert.Equal("Game Binaries", result.Manifest.LogicOutputPath);
            Assert.Equal("SandboxLogic", result.Manifest.LogicModuleName);
        }
        finally
        {
            if (File.Exists(path))
                File.Delete(path);
        }
    }

    [Fact]
    public void Deserialize_OldVersionOneManifestUsesMissingOnlyDefaults()
    {
        var serializer = new WorkspaceManifestSerializer();

        var result = serializer.Deserialize("""
manifestVersion: 1
workspaceId: workspace-id
name: Sandbox
enginePath: ../Sailor
contentPath: Content
sourcePath: Source
generatedProjectPath: Generated
cachePath: Cache
""");

        Assert.True(result.Succeeded);
        Assert.NotNull(result.Manifest);
        Assert.Equal(WorkspaceEngineReferenceKinds.Source, result.Manifest.EngineReferenceKind);
        Assert.Equal("Cache/Build", result.Manifest.BuildPath);
        Assert.Equal("Binaries", result.Manifest.LogicOutputPath);
        Assert.Equal("SailorGame", result.Manifest.LogicModuleName);
    }

    [Theory]
    [InlineData("engineReferenceKind", nameof(WorkspaceManifest.EngineReferenceKind), "")]
    [InlineData("engineReferenceKind", nameof(WorkspaceManifest.EngineReferenceKind), "\"\"")]
    [InlineData("buildPath", nameof(WorkspaceManifest.BuildPath), "")]
    [InlineData("buildPath", nameof(WorkspaceManifest.BuildPath), "\"\"")]
    [InlineData("logicOutputPath", nameof(WorkspaceManifest.LogicOutputPath), "")]
    [InlineData("logicOutputPath", nameof(WorkspaceManifest.LogicOutputPath), "\"\"")]
    [InlineData("logicModuleName", nameof(WorkspaceManifest.LogicModuleName), "")]
    [InlineData("logicModuleName", nameof(WorkspaceManifest.LogicModuleName), "\"\"")]
    public void Deserialize_VersionOneRejectsExplicitNullOrEmptyDefaultedFields(
        string yamlField,
        string manifestField,
        string yamlValue)
    {
        var serializer = new WorkspaceManifestSerializer();

        var result = serializer.Deserialize(ValidManifest($"{yamlField}: {yamlValue}"));

        Assert.False(result.Succeeded);
        Assert.NotNull(result.Manifest);
        Assert.Contains(result.Validation.Issues, issue => issue.Field == manifestField);
    }

    [Fact]
    public void Deserialize_ToleratesUnknownAdditiveVersionOneFields()
    {
        var serializer = new WorkspaceManifestSerializer();

        var result = serializer.Deserialize(ValidManifest("""
futureFeature:
  enabled: true
  options: [one, two]
"""));

        Assert.True(result.Succeeded, result.Error);
        Assert.NotNull(result.Manifest);
        Assert.Equal("workspace-id", result.Manifest.WorkspaceId);
    }

    [Theory]
    [InlineData("git", false)]
    [InlineData("source", true)]
    [InlineData("installed", true)]
    public void Validate_RequiresSupportedEngineReferenceKind(string engineReferenceKind, bool expectedValid)
    {
        var serializer = new WorkspaceManifestSerializer();
        var manifest = WorkspaceManifest.CreateDefault("Sandbox", "../Sailor", "workspace-id") with
        {
            EngineReferenceKind = engineReferenceKind
        };

        var validation = serializer.Validate(manifest);

        Assert.Equal(expectedValid, validation.IsValid);
        Assert.Equal(!expectedValid, validation.Issues.Any(x => x.Field == nameof(WorkspaceManifest.EngineReferenceKind)));
    }

    [Theory]
    [InlineData("SailorGame", true)]
    [InlineData("_Sandbox2", true)]
    [InlineData("2Sandbox", false)]
    [InlineData("Sandbox-Logic", false)]
    [InlineData("Sandbox Logic", false)]
    [InlineData("Module.Name", false)]
    public void Validate_RequiresCIdentifierForLogicModuleName(string moduleName, bool expectedValid)
    {
        var serializer = new WorkspaceManifestSerializer();
        var manifest = WorkspaceManifest.CreateDefault("Sandbox", "../Sailor", "workspace-id") with
        {
            LogicModuleName = moduleName
        };

        var validation = serializer.Validate(manifest);

        Assert.Equal(expectedValid, validation.IsValid);
        Assert.Equal(!expectedValid, validation.Issues.Any(x => x.Field == nameof(WorkspaceManifest.LogicModuleName)));
    }

    public static IEnumerable<object[]> UnsafeWorkspaceOwnedPaths()
    {
        var fields = new[]
        {
            nameof(WorkspaceManifest.ContentPath),
            nameof(WorkspaceManifest.SourcePath),
            nameof(WorkspaceManifest.GeneratedProjectPath),
            nameof(WorkspaceManifest.CachePath),
            nameof(WorkspaceManifest.BuildPath),
            nameof(WorkspaceManifest.LogicOutputPath)
        };
        var paths = new[]
        {
            "/absolute/path",
            "C:/absolute/path",
            "C:drive-relative",
            "../outside",
            "safe/../../outside",
            @"safe\..\outside",
            "Content\0Invalid"
        };

        return fields.SelectMany(field => paths.Select(path => new object[] { field, path }));
    }

    [Theory]
    [MemberData(nameof(UnsafeWorkspaceOwnedPaths))]
    public void Validate_RejectsUnsafeWorkspaceOwnedPaths(string field, string path)
    {
        var serializer = new WorkspaceManifestSerializer();
        var manifest = WithWorkspaceOwnedPath(
            WorkspaceManifest.CreateDefault("Sandbox", "../Sailor", "workspace-id"),
            field,
            path);

        var validation = serializer.Validate(manifest);

        Assert.False(validation.IsValid);
        Assert.Contains(validation.Issues, x => x.Field == field && x.Message.Contains("safe relative path", StringComparison.Ordinal));
    }

    [Theory]
    [InlineData("/opt/sailor")]
    [InlineData("C:/Sailor")]
    [InlineData("C:Sailor")]
    [InlineData("../Sailor")]
    public void Validate_AllowsExternalEnginePath(string enginePath)
    {
        var serializer = new WorkspaceManifestSerializer();
        var manifest = WorkspaceManifest.CreateDefault("Sandbox", enginePath, "workspace-id");

        var validation = serializer.Validate(manifest);

        Assert.True(validation.IsValid);
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

    [Theory]
    [MemberData(nameof(InvalidManifestVersionDocuments))]
    public void Deserialize_RawVersionProbeRejectsInvalidVersionContracts(
        string yaml,
        WorkspaceYamlVersionProbeFailure expectedFailure,
        string expectedDiagnostic)
    {
        var serializer = new WorkspaceManifestSerializer();

        var probe = WorkspaceYamlVersionProbe.Probe(
            yaml,
            "manifestVersion",
            WorkspaceManifest.CurrentVersion,
            "Workspace manifest");
        var result = serializer.Deserialize(yaml);

        Assert.False(probe.Succeeded);
        Assert.Equal(expectedFailure, probe.Failure);
        Assert.Contains(expectedDiagnostic, probe.Error, StringComparison.OrdinalIgnoreCase);
        Assert.False(result.Succeeded);
        Assert.Null(result.Manifest);
        var issue = Assert.Single(result.Validation.Issues);
        Assert.Equal(nameof(WorkspaceManifest.ManifestVersion), issue.Field);
        Assert.Equal(probe.Error, issue.Message);
    }

    [Fact]
    public void Deserialize_FutureVersionPrecedesMalformedLaterTypedFields()
    {
        var serializer = new WorkspaceManifestSerializer();

        var result = serializer.Deserialize("""
workspaceId: [not, a, string]
manifestVersion: 999
name:
  invalid: for-a-string
enginePath: ../Sailor
contentPath: [not, a, path]
""");

        Assert.False(result.Succeeded);
        Assert.Null(result.Manifest);
        Assert.Null(result.Error);
        var issue = Assert.Single(result.Validation.Issues);
        Assert.Equal(nameof(WorkspaceManifest.ManifestVersion), issue.Field);
        Assert.Contains("Unsupported workspace manifest version '999'", issue.Message, StringComparison.Ordinal);
        Assert.DoesNotContain("parse", issue.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void VersionProbe_UsesCallerProvidedVersionContract()
    {
        var result = WorkspaceYamlVersionProbe.Probe(
            "generatorSchemaVersion: 3",
            "generatorSchemaVersion",
            3,
            "Generated project state");

        Assert.True(result.Succeeded, result.Error);
        Assert.Equal(3, result.Version);
        Assert.Equal(WorkspaceYamlVersionProbeFailure.None, result.Failure);
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

    static WorkspaceManifest WithWorkspaceOwnedPath(WorkspaceManifest manifest, string field, string path)
        => field switch
        {
            nameof(WorkspaceManifest.ContentPath) => manifest with { ContentPath = path },
            nameof(WorkspaceManifest.SourcePath) => manifest with { SourcePath = path },
            nameof(WorkspaceManifest.GeneratedProjectPath) => manifest with { GeneratedProjectPath = path },
            nameof(WorkspaceManifest.CachePath) => manifest with { CachePath = path },
            nameof(WorkspaceManifest.BuildPath) => manifest with { BuildPath = path },
            nameof(WorkspaceManifest.LogicOutputPath) => manifest with { LogicOutputPath = path },
            _ => throw new ArgumentOutOfRangeException(nameof(field), field, null)
        };

    public static IEnumerable<object[]> InvalidManifestVersionDocuments()
    {
        yield return [
            """
workspaceId: workspace-id
name: Sandbox
""",
            WorkspaceYamlVersionProbeFailure.MissingVersion,
            "required"
        ];
        yield return [
            """
manifestVersion: 0
workspaceId: workspace-id
""",
            WorkspaceYamlVersionProbeFailure.InvalidVersion,
            "greater than zero"
        ];
        yield return [
            """
manifestVersion: 999
workspaceId: workspace-id
""",
            WorkspaceYamlVersionProbeFailure.UnsupportedVersion,
            "unsupported workspace manifest version"
        ];
        yield return [
            """
manifestVersion: 1
manifestVersion: 1
workspaceId: workspace-id
""",
            WorkspaceYamlVersionProbeFailure.DuplicateVersion,
            "duplicate field"
        ];
        yield return [
            """
manifestVersion: [1]
workspaceId: workspace-id
""",
            WorkspaceYamlVersionProbeFailure.NonScalarVersion,
            "scalar integer"
        ];
    }

    static string ValidManifest(string optionalYaml = "")
        => $$"""
manifestVersion: 1
workspaceId: workspace-id
name: Sandbox
enginePath: ../Sailor
contentPath: Content
sourcePath: Source
generatedProjectPath: Generated
cachePath: Cache
{{optionalYaml}}
""";
}
