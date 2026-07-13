using System.Diagnostics;
using SailorEditor.Workspace;

namespace SailorEditor.Editor.Tests;

public sealed class WorkspaceGeneratedProjectStateTests
{
    [Fact]
    public void Serialize_EmitsDeterministicSchemaAndOnlyGeneratorInputs()
    {
        var manifest = WorkspaceManifest.CreateDefault(
            "Workspace Name Must Not Be Recorded",
            "../Engine Root/",
            "workspace-id-must-not-be-recorded",
            WorkspaceEngineReferenceKinds.Installed) with
        {
            ContentPath = "Game/Content",
            SourcePath = "Game/Source",
            GeneratedProjectPath = "Project Files",
            CachePath = "Intermediate/Cache",
            BuildPath = "Intermediate/Build",
            LogicOutputPath = "Output/Binaries",
            LogicModuleName = "GameLogic"
        };
        var service = new WorkspaceGeneratedProjectStateService();

        var first = service.Serialize(manifest);
        var second = service.Serialize(manifest);

        Assert.Equal(first, second);
        Assert.DoesNotContain('\r', first);
        Assert.EndsWith("\n", first, StringComparison.Ordinal);
        Assert.Contains("generatorSchemaVersion: 1", first, StringComparison.Ordinal);
        Assert.Contains("creationInputs:", first, StringComparison.Ordinal);
        AssertFieldOrder(first, [
            "manifestVersion:",
            "enginePath:",
            "engineReferenceKind:",
            "contentPath:",
            "sourcePath:",
            "generatedProjectPath:",
            "cachePath:",
            "buildPath:",
            "logicOutputPath:",
            "logicModuleName:"
        ]);
        Assert.DoesNotContain("Workspace Name Must Not Be Recorded", first, StringComparison.Ordinal);
        Assert.DoesNotContain("workspace-id-must-not-be-recorded", first, StringComparison.Ordinal);
        Assert.DoesNotContain("workspaceRoot", first, StringComparison.Ordinal);
        Assert.DoesNotContain("timestamp", first, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("hash", first, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public async Task AssessAsync_ReturnsCurrentForEquivalentNormalizedInputs()
    {
        using var workspace = TempWorkspace.Create();
        var service = new WorkspaceGeneratedProjectStateService();
        var recorded = WorkspaceManifest.CreateDefault("Sandbox", "../Sailor/", "workspace-id") with
        {
            SourcePath = "./Game\\Source\\",
            BuildPath = ".\\Cache\\Build\\"
        };
        WriteState(workspace.Root, service.Serialize(recorded));
        var equivalent = recorded with
        {
            EnginePath = "../Sailor",
            SourcePath = "Game/Source",
            BuildPath = "Cache/Build"
        };

        var assessment = await service.AssessAsync(workspace.Root, equivalent);

        Assert.Equal(WorkspaceGeneratedProjectStateStatus.Current, assessment.Status);
        Assert.False(assessment.RequiresAttention);
        Assert.Empty(assessment.Guidance);
        Assert.Empty(assessment.Mismatches);
    }

    [Fact]
    public async Task AssessAsync_ReportsAllStaleInputsInContractOrderWithoutWriting()
    {
        using var workspace = TempWorkspace.Create();
        var service = new WorkspaceGeneratedProjectStateService();
        var recorded = WorkspaceManifest.CreateDefault("Recorded", "../RecordedEngine", "recorded-id");
        var statePath = WriteState(workspace.Root, service.Serialize(recorded));
        var before = await File.ReadAllBytesAsync(statePath);
        var current = recorded with
        {
            ManifestVersion = 2,
            EnginePath = "../CurrentEngine",
            EngineReferenceKind = WorkspaceEngineReferenceKinds.Installed,
            ContentPath = "Game/Content",
            SourcePath = "Game/Source",
            GeneratedProjectPath = "Project Files",
            CachePath = "Intermediate/Cache",
            BuildPath = "Intermediate/Build",
            LogicOutputPath = "Output/Binaries",
            LogicModuleName = "CurrentLogic"
        };

        var assessment = await service.AssessAsync(workspace.Root, current);

        Assert.Equal(WorkspaceGeneratedProjectStateStatus.Stale, assessment.Status);
        Assert.True(assessment.RequiresAttention);
        Assert.Equal(
            [
                "manifestVersion",
                "enginePath",
                "engineReferenceKind",
                "contentPath",
                "sourcePath",
                "generatedProjectPath",
                "cachePath",
                "buildPath",
                "logicOutputPath",
                "logicModuleName"
            ],
            assessment.Mismatches.Select(x => x.Field));
        Assert.Contains("Restore the recorded manifest values", assessment.Guidance, StringComparison.Ordinal);
        Assert.Contains("Sailor did not regenerate or overwrite anything", assessment.Guidance, StringComparison.Ordinal);
        Assert.Equal(before, await File.ReadAllBytesAsync(statePath));
    }

    [Fact]
    public async Task AssessAsync_PositiveNonCurrentRecordedManifestVersionIsStaleRatherThanInvalid()
    {
        using var workspace = TempWorkspace.Create();
        var service = new WorkspaceGeneratedProjectStateService();
        var current = WorkspaceManifest.CreateDefault("Sandbox", "../Sailor", "workspace-id");
        var yaml = service.Serialize(current).Replace(
            "manifestVersion: 1",
            "manifestVersion: 2",
            StringComparison.Ordinal);
        WriteState(workspace.Root, yaml);

        var assessment = await service.AssessAsync(workspace.Root, current);

        Assert.Equal(WorkspaceGeneratedProjectStateStatus.Stale, assessment.Status);
        var mismatch = Assert.Single(assessment.Mismatches);
        Assert.Equal("manifestVersion", mismatch.Field);
        Assert.Equal("2", mismatch.RecordedValue);
        Assert.Equal("1", mismatch.CurrentValue);
    }

    [Fact]
    public async Task AssessAsync_MissingStateIsUntrackedAndIsNotBackfilled()
    {
        using var workspace = TempWorkspace.Create();
        var service = new WorkspaceGeneratedProjectStateService();
        var manifest = WorkspaceManifest.CreateDefault("Sandbox", "../Sailor", "workspace-id");

        var assessment = await service.AssessAsync(workspace.Root, manifest);

        Assert.Equal(WorkspaceGeneratedProjectStateStatus.Untracked, assessment.Status);
        Assert.True(assessment.RequiresAttention);
        Assert.Empty(assessment.Mismatches);
        Assert.Contains("add the state file to source control", assessment.Guidance, StringComparison.Ordinal);
        Assert.Contains("did not backfill or overwrite anything", assessment.Guidance, StringComparison.Ordinal);
        Assert.False(Directory.Exists(workspace.Directory(WorkspaceGeneratedProjectStateService.StateDirectoryName)));
    }

    [Theory]
    [InlineData("generatorSchemaVersion: 0\ncreationInputs: {}\n", "greater than zero")]
    [InlineData("generatorSchemaVersion: 2\ncreationInputs: [future, schema]\n", "Unsupported generated project state version '2'")]
    [InlineData("creationInputs: {}\n", "generatorSchemaVersion")]
    public async Task AssessAsync_InvalidSchemaIsAdvisoryAndDoesNotRewrite(
        string yaml,
        string expectedGuidance)
    {
        using var workspace = TempWorkspace.Create();
        var service = new WorkspaceGeneratedProjectStateService();
        var manifest = WorkspaceManifest.CreateDefault("Sandbox", "../Sailor", "workspace-id");
        var statePath = WriteState(workspace.Root, yaml);
        var before = await File.ReadAllBytesAsync(statePath);

        var assessment = await service.AssessAsync(workspace.Root, manifest);

        Assert.Equal(WorkspaceGeneratedProjectStateStatus.Invalid, assessment.Status);
        Assert.True(assessment.RequiresAttention);
        Assert.Empty(assessment.Mismatches);
        Assert.Contains(expectedGuidance, assessment.Guidance, StringComparison.OrdinalIgnoreCase);
        Assert.Contains("did not backfill, regenerate, or overwrite anything", assessment.Guidance, StringComparison.Ordinal);
        Assert.Equal(before, await File.ReadAllBytesAsync(statePath));
    }

    [Fact]
    public async Task AssessAsync_IncompleteCurrentSchemaIsInvalidWithoutWriting()
    {
        using var workspace = TempWorkspace.Create();
        var service = new WorkspaceGeneratedProjectStateService();
        var manifest = WorkspaceManifest.CreateDefault("Sandbox", "../Sailor", "workspace-id");
        var statePath = WriteState(workspace.Root, "generatorSchemaVersion: 1\ncreationInputs:\n  manifestVersion: 1\n");
        var before = await File.ReadAllBytesAsync(statePath);

        var assessment = await service.AssessAsync(workspace.Root, manifest);

        Assert.Equal(WorkspaceGeneratedProjectStateStatus.Invalid, assessment.Status);
        Assert.Contains("complete schema v1 creation inputs", assessment.Guidance, StringComparison.Ordinal);
        Assert.Equal(before, await File.ReadAllBytesAsync(statePath));
    }

    public static IEnumerable<object[]> MalformedCreationInputs()
    {
        yield return ["manifestVersion"];
        yield return ["enginePath"];
        yield return ["engineReferenceKind"];
        yield return [nameof(WorkspaceManifest.ContentPath)];
        yield return [nameof(WorkspaceManifest.SourcePath)];
        yield return [nameof(WorkspaceManifest.GeneratedProjectPath)];
        yield return [nameof(WorkspaceManifest.CachePath)];
        yield return [nameof(WorkspaceManifest.BuildPath)];
        yield return [nameof(WorkspaceManifest.LogicOutputPath)];
        yield return ["logicModuleName"];
    }

    [Theory]
    [MemberData(nameof(MalformedCreationInputs))]
    public async Task AssessAsync_MalformedOrUnsafeRecordedInputsAreInvalid(string field)
    {
        using var workspace = TempWorkspace.Create();
        var service = new WorkspaceGeneratedProjectStateService();
        var current = WorkspaceManifest.CreateDefault("Sandbox", "../Sailor", "workspace-id");
        var recorded = field switch
        {
            "manifestVersion" => current with { ManifestVersion = 0 },
            "enginePath" => current with { EnginePath = "" },
            "engineReferenceKind" => current with { EngineReferenceKind = "git" },
            nameof(WorkspaceManifest.ContentPath) => current with { ContentPath = "../Content" },
            nameof(WorkspaceManifest.SourcePath) => current with { SourcePath = "../Source" },
            nameof(WorkspaceManifest.GeneratedProjectPath) => current with { GeneratedProjectPath = "../Generated" },
            nameof(WorkspaceManifest.CachePath) => current with { CachePath = "../Cache" },
            nameof(WorkspaceManifest.BuildPath) => current with { BuildPath = "../Build" },
            nameof(WorkspaceManifest.LogicOutputPath) => current with { LogicOutputPath = "../Binaries" },
            "logicModuleName" => current with { LogicModuleName = "Invalid-Module" },
            _ => throw new ArgumentOutOfRangeException(nameof(field), field, null)
        };
        var statePath = WriteState(workspace.Root, service.Serialize(recorded));
        var before = await File.ReadAllBytesAsync(statePath);

        var assessment = await service.AssessAsync(workspace.Root, current);

        Assert.Equal(WorkspaceGeneratedProjectStateStatus.Invalid, assessment.Status);
        Assert.Empty(assessment.Mismatches);
        Assert.Contains("malformed or unsafe creation inputs", assessment.Guidance, StringComparison.Ordinal);
        Assert.Equal(before, await File.ReadAllBytesAsync(statePath));
    }

    [Fact]
    public async Task AssessAsync_RejectsStateDirectoryLinkEscapeWithoutReadingOrWritingOutside()
    {
        using var workspace = TempWorkspace.Create();
        using var outside = TempWorkspace.Create();
        var service = new WorkspaceGeneratedProjectStateService();
        var manifest = WorkspaceManifest.CreateDefault("Sandbox", "../Sailor", "workspace-id");
        var outsideState = outside.File(WorkspaceGeneratedProjectStateService.StateFileName);
        await File.WriteAllTextAsync(outsideState, service.Serialize(manifest));
        var before = await File.ReadAllBytesAsync(outsideState);
        var linkPath = workspace.Directory(WorkspaceGeneratedProjectStateService.StateDirectoryName);
        CreateDirectoryLink(linkPath, outside.Root);

        try
        {
            var assessment = await service.AssessAsync(workspace.Root, manifest);

            Assert.Equal(WorkspaceGeneratedProjectStateStatus.Invalid, assessment.Status);
            Assert.Contains("safely contained", assessment.Guidance, StringComparison.Ordinal);
            Assert.Equal(before, await File.ReadAllBytesAsync(outsideState));
        }
        finally
        {
            if (Directory.Exists(linkPath))
                Directory.Delete(linkPath);
        }
    }

    static string WriteState(string workspaceRoot, string yaml)
    {
        var stateDirectory = Path.Combine(
            workspaceRoot,
            WorkspaceGeneratedProjectStateService.StateDirectoryName);
        Directory.CreateDirectory(stateDirectory);
        var statePath = Path.Combine(
            stateDirectory,
            WorkspaceGeneratedProjectStateService.StateFileName);
        File.WriteAllText(statePath, yaml);
        return statePath;
    }

    static void AssertFieldOrder(string yaml, IReadOnlyList<string> fields)
    {
        var previous = -1;
        foreach (var field in fields)
        {
            var current = yaml.IndexOf(field, StringComparison.Ordinal);
            Assert.True(current > previous, $"Expected '{field}' after the previous generated-state field.\n{yaml}");
            previous = current;
        }
    }

    static void CreateDirectoryLink(string linkPath, string targetPath)
    {
        if (!OperatingSystem.IsWindows())
        {
            Directory.CreateSymbolicLink(linkPath, targetPath);
            return;
        }

        var startInfo = new ProcessStartInfo("cmd.exe")
        {
            CreateNoWindow = true,
            RedirectStandardError = true,
            RedirectStandardOutput = true,
            UseShellExecute = false
        };
        startInfo.ArgumentList.Add("/d");
        startInfo.ArgumentList.Add("/c");
        startInfo.ArgumentList.Add("mklink");
        startInfo.ArgumentList.Add("/J");
        startInfo.ArgumentList.Add(linkPath);
        startInfo.ArgumentList.Add(targetPath);

        using var process = Process.Start(startInfo) ?? throw new InvalidOperationException("Could not start mklink.");
        process.WaitForExit();
        if (process.ExitCode != 0)
        {
            throw new InvalidOperationException(
                $"Could not create test junction: {process.StandardError.ReadToEnd()}{process.StandardOutput.ReadToEnd()}");
        }
    }

    sealed class TempWorkspace : IDisposable
    {
        TempWorkspace(string root)
        {
            Root = root;
            System.IO.Directory.CreateDirectory(root);
        }

        public string Root { get; }

        public static TempWorkspace Create()
            => new(Path.Combine(Path.GetTempPath(), $"sailor-generated-state-{Guid.NewGuid():N}"));

        public string File(string relativePath) => Path.Combine(Root, relativePath);

        public string Directory(string relativePath) => Path.Combine(Root, relativePath);

        public void Dispose()
        {
            if (System.IO.Directory.Exists(Root))
                System.IO.Directory.Delete(Root, recursive: true);
        }
    }
}
