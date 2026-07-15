using SailorEditor.Workspace;

namespace SailorEditor.Editor.Tests;

public sealed class WorkspaceProjectGeneratorTests
{
    [Fact]
    public async Task GenerateAsync_CreatesSourceEngineProjectWithExpectedLayout()
    {
        using var workspace = TempWorkspace.Create(includeSpaces: true);
        using var engine = TempWorkspace.Create(includeSpaces: true);
        var manifest = WorkspaceManifest.CreateDefault("Sandbox", engine.Root, "workspace-id");
        var session = CreateSession(workspace.Root, manifest);
        var generator = new WorkspaceProjectGenerator();

        var generated = await generator.GenerateAsync(session);

        var cmake = await File.ReadAllTextAsync(workspace.File("Generated/CMakeLists.txt"));
        Assert.Contains($"add_subdirectory(\"{ToCMakePath(engine.Root)}\" \"${{CMAKE_BINARY_DIR}}/SailorEngine\" EXCLUDE_FROM_ALL)", cmake);
        Assert.Contains("set(SAILOR_BUILD_EXECUTABLE OFF", cmake);
        Assert.Contains("set(SAILOR_BUILD_TESTS OFF", cmake);
        Assert.Contains("target_link_libraries(SailorGame PRIVATE Sailor::Runtime)", cmake);
        Assert.Contains("SAILOR_WORKSPACE_MODULE", cmake);
        Assert.Contains("SAILOR_WORKSPACE_MODULE_EXPORTS", cmake);
        Assert.DoesNotContain("WINDOWS_EXPORT_ALL_SYMBOLS", cmake);
        Assert.DoesNotContain("if(NOT WIN32)", cmake);
        Assert.Contains("if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)", cmake);
        Assert.Contains("CMAKE_CXX_COMPILER_ID MATCHES \"AppleClang|Clang|GNU\"", cmake);
        Assert.Contains("PDB_OUTPUT_DIRECTORY \"${SAILOR_GAME_OUTPUT_DIR}/$<CONFIG>\"", cmake);
        Assert.Contains("COMPILE_PDB_OUTPUT_DIRECTORY \"${SAILOR_GAME_OUTPUT_DIR}/$<CONFIG>\"", cmake);
        Assert.Contains("${CMAKE_CURRENT_LIST_DIR}/../Source", cmake);
        Assert.Contains("${CMAKE_CURRENT_LIST_DIR}/../Binaries", cmake);
        Assert.True(File.Exists(workspace.File("Source/Components/SampleComponent.h")));
        Assert.True(File.Exists(workspace.File("Source/Components/SampleComponent.cpp")));
        Assert.True(File.Exists(workspace.File("Source/WorkspaceTypes.h")));
        Assert.True(File.Exists(workspace.File("Source/WorkspaceModule.cpp")));
        Assert.True(File.Exists(workspace.File(".gitignore")));
        var generatedState = new WorkspaceGeneratedProjectStateService().GetStatePath(workspace.Root);
        Assert.True(File.Exists(generatedState));
        Assert.Equal(generatedState, generated.CreatedFiles.Last());
        Assert.Equal(
            WorkspaceGeneratedProjectStateStatus.Current,
            (await new WorkspaceGeneratedProjectStateService().AssessAsync(workspace.Root, manifest)).Status);
        Assert.True(Directory.Exists(workspace.Directory("Cache/Build")));
        Assert.True(Directory.Exists(workspace.Directory("Binaries")));
    }

    [Fact]
    public async Task GenerateAsync_CreatesInstalledEngineProjectWithCustomPaths()
    {
        using var workspace = TempWorkspace.Create(includeSpaces: true);
        using var engineInstall = TempWorkspace.Create(includeSpaces: true);
        var manifest = WorkspaceManifest.CreateDefault(
            "Sandbox",
            engineInstall.Root,
            "workspace-id",
            WorkspaceEngineReferenceKinds.Installed) with
        {
            SourcePath = "Game Source",
            GeneratedProjectPath = "Project Files",
            BuildPath = "Intermediate/Build Files",
            LogicOutputPath = "Output Files",
            LogicModuleName = "GameLogic"
        };
        var session = CreateSession(workspace.Root, manifest);
        var generator = new WorkspaceProjectGenerator();

        await generator.GenerateAsync(session);

        var cmake = await File.ReadAllTextAsync(workspace.File("Project Files/CMakeLists.txt"));
        Assert.Contains($"list(PREPEND CMAKE_PREFIX_PATH \"{ToCMakePath(engineInstall.Root)}\")", cmake);
        Assert.Contains("find_package(Sailor CONFIG REQUIRED)", cmake);
        Assert.DoesNotContain("add_subdirectory", cmake);
        Assert.Contains("project(GameLogic LANGUAGES CXX)", cmake);
        Assert.Contains("${CMAKE_CURRENT_LIST_DIR}/../Game Source", cmake);
        Assert.Contains("${CMAKE_CURRENT_LIST_DIR}/../Output Files", cmake);
        Assert.True(Directory.Exists(workspace.Directory("Intermediate/Build Files")));
        Assert.True(Directory.Exists(workspace.Directory("Output Files")));
    }

    [Fact]
    public async Task GenerateAsync_DoesNotOverwriteAnyExistingProjectFile()
    {
        using var workspace = TempWorkspace.Create();
        var componentsDirectory = workspace.Directory("Source/Components");
        Directory.CreateDirectory(componentsDirectory);
        var existingHeader = workspace.File("Source/Components/SampleComponent.h");
        await File.WriteAllTextAsync(existingHeader, "user-owned content");
        var manifest = WorkspaceManifest.CreateDefault("Sandbox", "../Sailor", "workspace-id");
        var session = CreateSession(workspace.Root, manifest);
        var generator = new WorkspaceProjectGenerator();

        var error = await Assert.ThrowsAsync<InvalidOperationException>(() => generator.GenerateAsync(session));

        Assert.Contains("will not be overwritten", error.Message, StringComparison.Ordinal);
        Assert.Equal("user-owned content", await File.ReadAllTextAsync(existingHeader));
        Assert.False(File.Exists(workspace.File(".gitignore")));
        Assert.False(File.Exists(workspace.File("Generated/CMakeLists.txt")));
    }

    [Fact]
    public async Task GenerateAsync_DoesNotOverwriteExistingGeneratedState()
    {
        using var workspace = TempWorkspace.Create();
        var stateDirectory = workspace.Directory(WorkspaceGeneratedProjectStateService.StateDirectoryName);
        Directory.CreateDirectory(stateDirectory);
        var statePath = workspace.File(WorkspaceGeneratedProjectStateService.RelativePath);
        await File.WriteAllTextAsync(statePath, "source-controlled user state");
        var manifest = WorkspaceManifest.CreateDefault("Sandbox", "../Sailor", "workspace-id");
        var session = CreateSession(workspace.Root, manifest);
        var generator = new WorkspaceProjectGenerator();

        var error = await Assert.ThrowsAsync<InvalidOperationException>(() => generator.GenerateAsync(session));

        Assert.Contains("will not be overwritten", error.Message, StringComparison.Ordinal);
        Assert.Equal("source-controlled user state", await File.ReadAllTextAsync(statePath));
        Assert.False(File.Exists(workspace.File(".gitignore")));
        Assert.False(File.Exists(workspace.File("Generated/CMakeLists.txt")));
        Assert.False(File.Exists(workspace.File("Source/WorkspaceModule.cpp")));
    }

    [Fact]
    public async Task GenerateAsync_PreflightsDirectoryCollisionWithGeneratedStateWithoutMutation()
    {
        using var workspace = TempWorkspace.Create();
        var manifest = WorkspaceManifest.CreateDefault("Sandbox", "../Sailor", "workspace-id") with
        {
            CachePath = WorkspaceGeneratedProjectStateService.RelativePath
        };
        var session = CreateSession(workspace.Root, manifest);

        var error = await Assert.ThrowsAsync<InvalidOperationException>(() =>
            new WorkspaceProjectGenerator().GenerateAsync(session));

        Assert.Contains("conflicts with generated file", error.Message, StringComparison.Ordinal);
        Assert.Empty(Directory.EnumerateFileSystemEntries(workspace.Root));
    }

    [Fact]
    public async Task GenerateAsync_DoesNotIgnoreStateDirectoryWhenCacheUsesSamePath()
    {
        using var workspace = TempWorkspace.Create();
        var manifest = WorkspaceManifest.CreateDefault("Sandbox", "../Sailor", "workspace-id") with
        {
            CachePath = WorkspaceGeneratedProjectStateService.StateDirectoryName,
            BuildPath = WorkspaceGeneratedProjectStateService.StateDirectoryName + "/Build"
        };
        var session = CreateSession(workspace.Root, manifest);

        await new WorkspaceProjectGenerator().GenerateAsync(session);

        var ignoredLines = (await File.ReadAllLinesAsync(workspace.File(".gitignore")))
            .Where(x => !string.IsNullOrWhiteSpace(x))
            .ToArray();
        Assert.Contains("/.sailor/*", ignoredLines);
        Assert.Contains("!/.sailor/GeneratedProjectState.yaml", ignoredLines);
        Assert.DoesNotContain("/.sailor/", ignoredLines);
        Assert.Contains("/.sailor/Build/", ignoredLines);
        Assert.True(File.Exists(workspace.File(WorkspaceGeneratedProjectStateService.RelativePath)));
    }

    [Fact]
    public async Task GenerateAsync_EmitsEditableSampleComponent()
    {
        using var workspace = TempWorkspace.Create();
        var manifest = WorkspaceManifest.CreateDefault("Sandbox", "../Sailor", "workspace-id") with
        {
            LogicModuleName = "SandboxLogic"
        };
        var session = CreateSession(workspace.Root, manifest);
        var generator = new WorkspaceProjectGenerator();

        await generator.GenerateAsync(session);

        var header = await File.ReadAllTextAsync(workspace.File("Source/Components/SampleComponent.h"));
        var source = await File.ReadAllTextAsync(workspace.File("Source/Components/SampleComponent.cpp"));
        var workspaceTypes = await File.ReadAllTextAsync(workspace.File("Source/WorkspaceTypes.h"));
        var workspaceModule = await File.ReadAllTextAsync(workspace.File("Source/WorkspaceModule.cpp"));
        Assert.Contains("namespace SandboxLogic", header);
        Assert.Contains("class SampleComponent final : public Sailor::Component", header);
        Assert.Contains("SAILOR_WORKSPACE_REFLECTABLE(SampleComponent)", header);
        Assert.Contains("float m_moveSpeed = 5.0f", header);
        Assert.Contains("func(GetMoveSpeed, property(\"moveSpeed\"))", header);
        Assert.Contains("void SandboxLogic::SampleComponent::BeginPlay()", source);
        Assert.Contains("TWorkspaceTypeList<SampleComponent>", workspaceTypes);
        Assert.Contains("SailorGetWorkspaceTypeMetadataV1", workspaceModule);
        Assert.Contains("ExportWorkspaceTypeMetadataV1<SandboxLogic::WorkspaceTypes>", workspaceModule);
        Assert.Contains("constexpr char WorkspaceModuleName[] = \"SandboxLogic\"", workspaceModule);
        Assert.Contains("SailorGetWorkspaceModuleApiV1", workspaceModule);
        Assert.Contains("WorkspaceModuleApiVersion", workspaceModule);
        Assert.Contains("GetWorkspaceModuleAbiTagV1()", workspaceModule);
        Assert.Contains("RegisterWorkspaceTypesV1<SandboxLogic::WorkspaceTypes>", workspaceModule);
        Assert.Contains("&SailorGetWorkspaceTypeMetadataV1", workspaceModule);
        Assert.Contains("&RegisterWorkspaceTypes", workspaceModule);
    }

    [Fact]
    public async Task Rollback_RemovesTrackedArtifactsAndPreservesUntrackedContent()
    {
        using var workspace = TempWorkspace.Create();
        var sourceDirectory = workspace.Directory("Source");
        var componentsDirectory = workspace.Directory("Source/Components");
        var generatedDirectory = workspace.Directory("Generated");
        var stateDirectory = workspace.Directory(WorkspaceGeneratedProjectStateService.StateDirectoryName);
        Directory.CreateDirectory(componentsDirectory);
        Directory.CreateDirectory(generatedDirectory);
        Directory.CreateDirectory(stateDirectory);
        var trackedFile = workspace.File("Source/Components/SampleComponent.h");
        var trackedState = workspace.File(WorkspaceGeneratedProjectStateService.RelativePath);
        var untrackedFile = workspace.File("Source/UserComponent.cpp");
        await File.WriteAllTextAsync(trackedFile, "generated");
        await File.WriteAllTextAsync(trackedState, "generated state");
        await File.WriteAllTextAsync(untrackedFile, "user-owned");

        WorkspaceProjectGenerator.Rollback(new WorkspaceProjectGenerationResult(
            [trackedFile, trackedState],
            [sourceDirectory, componentsDirectory, generatedDirectory, stateDirectory]));

        Assert.False(File.Exists(trackedFile));
        Assert.False(File.Exists(trackedState));
        Assert.True(File.Exists(untrackedFile));
        Assert.True(Directory.Exists(sourceDirectory));
        Assert.False(Directory.Exists(generatedDirectory));
        Assert.False(Directory.Exists(stateDirectory));
    }

    static WorkspaceSession CreateSession(string workspaceRoot, WorkspaceManifest manifest)
    {
        var serializer = new WorkspaceManifestSerializer();
        return new WorkspaceTemplateService(serializer).CreateSession(workspaceRoot, manifest);
    }

    static string ToCMakePath(string path)
        => Path.GetFullPath(path).Replace('\\', '/');

    sealed class TempWorkspace : IDisposable
    {
        TempWorkspace(string root)
        {
            Root = root;
            System.IO.Directory.CreateDirectory(root);
        }

        public string Root { get; }

        public static TempWorkspace Create(bool includeSpaces = false)
        {
            var prefix = includeSpaces ? "sailor workspace" : "sailor-workspace";
            return new TempWorkspace(Path.Combine(Path.GetTempPath(), $"{prefix}-{Guid.NewGuid():N}"));
        }

        public string File(string relativePath) => Path.Combine(Root, relativePath);

        public string Directory(string relativePath) => Path.Combine(Root, relativePath);

        public void Dispose()
        {
            if (System.IO.Directory.Exists(Root))
                System.IO.Directory.Delete(Root, recursive: true);
        }
    }
}

public sealed class WorkspaceProjectGeneratorIntegrationTests
{
    [Fact]
    [Trait("Category", "CMakeIntegration")]
    public async Task SourceMode_ConfiguresBuildsAndEmitsReleaseDll()
    {
        if (!WorkspaceCMakeIntegrationHarness.ShouldRun)
            return;

        WorkspaceCMakeIntegrationHarness.EnsureWindows();
        var engineRoot = WorkspaceCMakeIntegrationHarness.GetRequiredEnvironmentVariable(
            "SAILOR_ENGINE_SOURCE_DIR");
        await GenerateBuildAndAssertAsync(
            engineRoot,
            WorkspaceEngineReferenceKinds.Source,
            "SailorSourceGame");
    }

    static async Task GenerateBuildAndAssertAsync(
        string enginePath,
        string engineReferenceKind,
        string moduleName)
    {
        using var workspace = IntegrationWorkspace.Create();
        var manifest = WorkspaceManifest.CreateDefault(
            "Integration",
            Path.GetFullPath(enginePath),
            "integration-workspace",
            engineReferenceKind) with
        {
            LogicModuleName = moduleName
        };
        var serializer = new WorkspaceManifestSerializer();
        var session = new WorkspaceTemplateService(serializer).CreateSession(workspace.Root, manifest);
        await new WorkspaceProjectGenerator().GenerateAsync(session);
        await WorkspaceCMakeIntegrationHarness.ConfigureBuildAndAssertAsync(session);
    }

    sealed class IntegrationWorkspace : IDisposable
    {
        IntegrationWorkspace(string root)
        {
            Root = root;
            Directory.CreateDirectory(root);
        }

        public string Root { get; }

        public static IntegrationWorkspace Create()
            => new(Path.Combine(Path.GetTempPath(), $"sailor cmake integration-{Guid.NewGuid():N}"));

        public void Dispose()
        {
            if (Directory.Exists(Root))
                Directory.Delete(Root, recursive: true);
        }
    }
}
