using System.Diagnostics;
using SailorEditor.Workspace;

namespace SailorEditor.Editor.Tests;

public sealed class WorkspaceLifecycleTests
{
    [Fact]
    public async Task CreateAsync_CreatesManifestAndExpectedFolders()
    {
        using var workspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var service = CreateService(recent.File("recent.yaml"));

        var result = await service.CreateAsync(new WorkspaceCreateRequest("Sandbox", workspace.Root, "../Sailor", "workspace-id"));

        Assert.True(result.Succeeded, result.Error);
        Assert.NotNull(result.Session);
        Assert.True(File.Exists(workspace.File(WorkspaceTemplateService.ManifestFileName)));
        Assert.True(Directory.Exists(workspace.Directory("Content")));
        Assert.True(Directory.Exists(workspace.Directory("Source")));
        Assert.True(Directory.Exists(workspace.Directory("Generated")));
        Assert.True(Directory.Exists(workspace.Directory("Cache")));
        Assert.True(Directory.Exists(workspace.Directory("Cache/Build")));
        Assert.True(Directory.Exists(workspace.Directory("Binaries")));
        Assert.True(File.Exists(workspace.File(".gitignore")));
        Assert.True(File.Exists(workspace.File("Generated/CMakeLists.txt")));
        Assert.True(File.Exists(workspace.File("Source/Components/SampleComponent.h")));
        Assert.True(File.Exists(workspace.File("Source/Components/SampleComponent.cpp")));
        Assert.True(File.Exists(workspace.File("Source/WorkspaceTypes.h")));
        Assert.True(File.Exists(workspace.File("Source/WorkspaceModule.cpp")));
        Assert.True(File.Exists(workspace.File(WorkspaceGeneratedProjectStateService.RelativePath)));
        Assert.Equal("workspace-id", result.Session.Manifest.WorkspaceId);
        Assert.Equal(Physical(workspace.Directory("Cache/Build")), result.Session.BuildDirectory);
        Assert.Equal(Physical(workspace.Directory("Binaries")), result.Session.LogicOutputDirectory);
        Assert.Equal(WorkspaceGeneratedProjectStateStatus.Current, result.Session.GeneratedProjectState.Status);
    }

    [Fact]
    public async Task OpenAsync_LoadsExistingWorkspaceManifest()
    {
        using var workspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var service = CreateService(recent.File("recent.yaml"));
        var created = await service.CreateAsync(new WorkspaceCreateRequest("Sandbox", workspace.Root, "../Sailor", "workspace-id"));

        var reopened = await service.OpenAsync(created.Session!.ManifestPath);

        Assert.True(reopened.Succeeded, reopened.Error);
        Assert.NotNull(reopened.Session);
        Assert.Equal(created.Session.ManifestPath, reopened.Session.ManifestPath);
        Assert.Equal("Sandbox", reopened.Session.Manifest.Name);
        Assert.Equal(Physical(workspace.Directory("Content")), reopened.Session.ContentDirectory);
    }

    [Fact]
    public async Task OpenAndSave_UntrackedGeneratedStateRemainNonBlockingWithoutBackfillOrProjectWrites()
    {
        using var workspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var service = CreateService(recent.File("recent.yaml"));
        var created = await service.CreateAsync(new WorkspaceCreateRequest(
            "Sandbox",
            workspace.Root,
            "../Sailor",
            "workspace-id"));
        var generatedStatePath = workspace.File(WorkspaceGeneratedProjectStateService.RelativePath);
        File.Delete(generatedStatePath);
        var ownedProjectFiles = new[]
        {
            workspace.File("Generated/CMakeLists.txt"),
            workspace.File("Source/Components/SampleComponent.h"),
            workspace.File("Source/Components/SampleComponent.cpp"),
            workspace.File("Source/WorkspaceTypes.h"),
            workspace.File("Source/WorkspaceModule.cpp")
        };
        var bytesBefore = ownedProjectFiles.ToDictionary(path => path, File.ReadAllBytes);

        var opened = await service.OpenAsync(created.Session!.ManifestPath);
        var saved = await service.SaveAsync(Assert.IsType<WorkspaceSession>(opened.Session));

        Assert.True(opened.Succeeded, opened.Error);
        Assert.True(saved.Succeeded, saved.Error);
        Assert.Equal(WorkspaceGeneratedProjectStateStatus.Untracked, opened.Session!.GeneratedProjectState.Status);
        Assert.Equal(WorkspaceGeneratedProjectStateStatus.Untracked, saved.Session!.GeneratedProjectState.Status);
        Assert.True(opened.Session.GeneratedProjectState.RequiresAttention);
        Assert.False(File.Exists(generatedStatePath));
        foreach (var path in ownedProjectFiles)
            Assert.Equal(bytesBefore[path], await File.ReadAllBytesAsync(path));
    }

    [Fact]
    public async Task SaveAsync_ChangedCreationInputLeavesGeneratedStateStaleAndUserProjectUntouched()
    {
        using var workspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var service = CreateService(recent.File("recent.yaml"));
        var created = await service.CreateAsync(new WorkspaceCreateRequest(
            "Sandbox",
            workspace.Root,
            "../Sailor",
            "workspace-id"));
        var session = Assert.IsType<WorkspaceSession>(created.Session);
        var generatedStatePath = workspace.File(WorkspaceGeneratedProjectStateService.RelativePath);
        var stateBefore = await File.ReadAllBytesAsync(generatedStatePath);
        var cmakePath = workspace.File("Generated/CMakeLists.txt");
        var cmakeBefore = await File.ReadAllBytesAsync(cmakePath);
        var changed = session with
        {
            Manifest = session.Manifest with { LogicModuleName = "RenamedGame" }
        };

        var result = await service.SaveAsync(changed);

        Assert.True(result.Succeeded, result.Error);
        Assert.Equal(WorkspaceGeneratedProjectStateStatus.Stale, result.Session!.GeneratedProjectState.Status);
        var mismatch = Assert.Single(result.Session.GeneratedProjectState.Mismatches);
        Assert.Equal("logicModuleName", mismatch.Field);
        Assert.Equal("SailorGame", mismatch.RecordedValue);
        Assert.Equal("RenamedGame", mismatch.CurrentValue);
        Assert.Equal(stateBefore, await File.ReadAllBytesAsync(generatedStatePath));
        Assert.Equal(cmakeBefore, await File.ReadAllBytesAsync(cmakePath));
    }

    [Fact]
    public async Task OpenAsync_InvalidOrEscapedGeneratedStateIsAdvisoryAndReadOnly()
    {
        using var workspace = TempWorkspace.Create();
        using var outside = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var service = CreateService(recent.File("recent.yaml"));
        var created = await service.CreateAsync(new WorkspaceCreateRequest(
            "Sandbox",
            workspace.Root,
            "../Sailor",
            "workspace-id"));
        var stateDirectory = workspace.Directory(".sailor");
        Directory.Delete(stateDirectory, recursive: true);
        var outsideState = outside.File("GeneratedProjectState.yaml");
        const string invalidState = "generatorSchemaVersion: 999\ncreationInputs: {}\n";
        await File.WriteAllTextAsync(outsideState, invalidState);
        CreateDirectoryLink(stateDirectory, outside.Root);

        try
        {
            var result = await service.OpenAsync(created.Session!.ManifestPath);

            Assert.True(result.Succeeded, result.Error);
            Assert.Equal(WorkspaceGeneratedProjectStateStatus.Invalid, result.Session!.GeneratedProjectState.Status);
            Assert.True(result.Session.GeneratedProjectState.RequiresAttention);
            Assert.Equal(invalidState, await File.ReadAllTextAsync(outsideState));
        }
        finally
        {
            if (Directory.Exists(stateDirectory))
                Directory.Delete(stateDirectory);
        }
    }

    [Fact]
    public async Task CreateAsync_UsesRequestedManifestPath()
    {
        using var workspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var service = CreateService(recent.File("recent.yaml"));
        var manifestPath = workspace.File("Sandbox.sailor");

        var result = await service.CreateAsync(new WorkspaceCreateRequest("Sandbox", workspace.Root, "../Sailor", "workspace-id", manifestPath));

        Assert.True(result.Succeeded, result.Error);
        Assert.Equal(Physical(manifestPath), result.Session!.ManifestPath);
        Assert.True(File.Exists(manifestPath));
        Assert.False(File.Exists(workspace.File(WorkspaceTemplateService.ManifestFileName)));
    }

    [Fact]
    public async Task CreateAsync_UsesRequestedInstalledEngineReference()
    {
        using var workspace = TempWorkspace.Create();
        using var engineInstall = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var service = CreateService(recent.File("recent.yaml"));

        var result = await service.CreateAsync(new WorkspaceCreateRequest(
            "Sandbox",
            workspace.Root,
            engineInstall.Root,
            "workspace-id",
            EngineReferenceKind: WorkspaceEngineReferenceKinds.Installed));

        Assert.True(result.Succeeded, result.Error);
        Assert.Equal(WorkspaceEngineReferenceKinds.Installed, result.Session!.Manifest.EngineReferenceKind);
        var cmake = await File.ReadAllTextAsync(workspace.File("Generated/CMakeLists.txt"));
        Assert.Contains("find_package(Sailor CONFIG REQUIRED)", cmake);
        Assert.DoesNotContain("add_subdirectory", cmake);
    }

    [Fact]
    public async Task CreateAsync_AllowsSelectedManifestPlaceholder()
    {
        using var workspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var service = CreateService(recent.File("recent.yaml"));
        var manifestPath = workspace.File("Sandbox.sailor");
        await File.WriteAllTextAsync(manifestPath, string.Empty);

        var result = await service.CreateAsync(new WorkspaceCreateRequest("Sandbox", workspace.Root, "../Sailor", "workspace-id", manifestPath));

        Assert.True(result.Succeeded, result.Error);
        Assert.Equal("Sandbox", result.Session!.Manifest.Name);
        Assert.Contains("manifestVersion", await File.ReadAllTextAsync(manifestPath));
    }

    [Fact]
    public async Task CreateAsync_InvalidRequestLeavesWorkspaceReusable()
    {
        using var workspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var service = CreateService(recent.File("recent.yaml"));

        var failed = await service.CreateAsync(new WorkspaceCreateRequest(
            "Sandbox",
            workspace.Root,
            "../Sailor",
            "workspace-id",
            EngineReferenceKind: "unsupported"));
        var retried = await service.CreateAsync(new WorkspaceCreateRequest(
            "Sandbox",
            workspace.Root,
            "../Sailor",
            "workspace-id"));

        Assert.False(failed.Succeeded);
        Assert.Contains("EngineReferenceKind", failed.Error, StringComparison.Ordinal);
        Assert.True(retried.Succeeded, retried.Error);
    }

    [Fact]
    public async Task CreateAsync_CancellationRollsBackAndRestoresManifestPlaceholder()
    {
        using var workspace = TempWorkspace.Create();
        var serializer = new WorkspaceManifestSerializer();
        var template = new WorkspaceTemplateService(serializer);
        var manifestPath = workspace.File("Sandbox.sailor");
        await File.WriteAllTextAsync(manifestPath, "selected manifest placeholder");
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();

        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => template.CreateAsync(
            new WorkspaceCreateRequest("Sandbox", workspace.Root, "../Sailor", "workspace-id", manifestPath),
            cancellation.Token));

        Assert.Equal("selected manifest placeholder", await File.ReadAllTextAsync(manifestPath));
        Assert.Equal(manifestPath, Assert.Single(Directory.EnumerateFileSystemEntries(workspace.Root)));
    }

    [Fact]
    public async Task CreateAsync_ManifestWriteFailureRollsBackGeneratedStateAndProjectFiles()
    {
        using var workspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var service = CreateService(recent.File("recent.yaml"));
        var manifestPath = workspace.Directory("Blocked.sailor");
        Directory.CreateDirectory(manifestPath);

        var result = await service.CreateAsync(new WorkspaceCreateRequest(
            "Sandbox",
            workspace.Root,
            "../Sailor",
            "workspace-id",
            manifestPath));

        Assert.False(result.Succeeded);
        Assert.True(Directory.Exists(manifestPath));
        Assert.Equal(manifestPath, Assert.Single(Directory.EnumerateFileSystemEntries(workspace.Root)));
        Assert.False(File.Exists(workspace.File(".gitignore")));
        Assert.False(File.Exists(workspace.File(WorkspaceGeneratedProjectStateService.RelativePath)));
        Assert.False(Directory.Exists(workspace.Directory(WorkspaceGeneratedProjectStateService.StateDirectoryName)));
        Assert.False(Directory.Exists(workspace.Directory("Content")));
        Assert.False(Directory.Exists(workspace.Directory("Source")));
        Assert.False(Directory.Exists(workspace.Directory("Generated")));
        Assert.False(Directory.Exists(workspace.Directory("Cache")));
        Assert.False(Directory.Exists(workspace.Directory("Binaries")));
    }

    [Fact]
    public async Task OpenAsync_PreservesOpenedManifestPath()
    {
        using var workspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var serializer = new WorkspaceManifestSerializer();
        var service = CreateService(recent.File("recent.yaml"));
        var manifestPath = workspace.File("custom.sailor");
        await serializer.SaveAsync(manifestPath, WorkspaceManifest.CreateDefault("Sandbox", "../Sailor", "workspace-id"));

        var result = await service.OpenAsync(manifestPath);

        Assert.True(result.Succeeded, result.Error);
        Assert.Equal(Physical(manifestPath), result.Session!.ManifestPath);
    }

    [Fact]
    public async Task OpenAsync_ResolvesContentDirectoryFromManifest()
    {
        using var workspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var serializer = new WorkspaceManifestSerializer();
        var service = CreateService(recent.File("recent.yaml"));
        var manifestPath = workspace.File("custom.sailor");
        var manifest = WorkspaceManifest.CreateDefault("Sandbox", "../Sailor", "workspace-id") with
        {
            ContentPath = "ProjectContent"
        };
        Directory.CreateDirectory(workspace.Directory("ProjectContent"));
        await serializer.SaveAsync(manifestPath, manifest);

        var result = await service.OpenAsync(manifestPath);

        Assert.True(result.Succeeded, result.Error);
        Assert.Equal(Physical(workspace.Directory("ProjectContent")), result.Session!.ContentDirectory);
    }

    [Fact]
    public async Task OpenAsync_RecoversMissingDefaultContentAndCacheDirectories()
    {
        using var workspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var service = CreateService(recent.File("recent.yaml"));
        var created = await service.CreateAsync(new WorkspaceCreateRequest("Sandbox", workspace.Root, "../Sailor", "workspace-id"));
        Directory.Delete(workspace.Directory("Content"), recursive: true);
        Directory.Delete(workspace.Directory("Cache"), recursive: true);

        var reopened = await service.OpenAsync(created.Session!.ManifestPath);

        Assert.True(reopened.Succeeded, reopened.Error);
        Assert.True(Directory.Exists(workspace.Directory("Content")));
        Assert.True(Directory.Exists(workspace.Directory("Cache")));
    }

    [Fact]
    public async Task OpenAsync_RecoversMissingCustomCacheDirectory()
    {
        using var workspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var serializer = new WorkspaceManifestSerializer();
        var service = CreateService(recent.File("recent.yaml"));
        var manifestPath = workspace.File("workspace.sailor");
        var manifest = WorkspaceManifest.CreateDefault("Sandbox", "../Sailor", "workspace-id") with
        {
            CachePath = "Generated Cache"
        };
        await serializer.SaveAsync(manifestPath, manifest);

        var reopened = await service.OpenAsync(manifestPath);

        Assert.True(reopened.Succeeded, reopened.Error);
        Assert.True(Directory.Exists(workspace.Directory("Content")));
        Assert.True(Directory.Exists(workspace.Directory("Generated Cache")));
    }

    [Fact]
    public async Task OpenAsync_RejectsMissingCustomContentWithoutCreatingDirectories()
    {
        using var workspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var serializer = new WorkspaceManifestSerializer();
        var service = CreateService(recent.File("recent.yaml"));
        var manifestPath = workspace.File("workspace.sailor");
        var manifest = WorkspaceManifest.CreateDefault("Sandbox", "../Sailor", "workspace-id") with
        {
            ContentPath = "ProjectContent"
        };
        await serializer.SaveAsync(manifestPath, manifest);

        var result = await service.OpenAsync(manifestPath);

        Assert.False(result.Succeeded);
        Assert.Contains("custom ContentPath directory does not exist", result.Error, StringComparison.Ordinal);
        Assert.False(Directory.Exists(workspace.Directory("ProjectContent")));
        Assert.False(Directory.Exists(workspace.Directory("Cache")));
        Assert.Null(service.Current);
    }

    [Fact]
    public async Task OpenAsync_RollsBackRecoveredContentWhenNestedCacheCreationFails()
    {
        using var workspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var serializer = new WorkspaceManifestSerializer();
        var service = CreateService(recent.File("recent.yaml"));
        var manifestPath = workspace.File("workspace.sailor");
        var manifest = WorkspaceManifest.CreateDefault("Sandbox", "../Sailor", "workspace-id") with
        {
            CachePath = "Blocked/Cache"
        };
        await serializer.SaveAsync(manifestPath, manifest);
        File.WriteAllText(workspace.File("Blocked"), "prevents cache directory creation");

        var result = await service.OpenAsync(manifestPath);

        Assert.False(result.Succeeded);
        Assert.False(Directory.Exists(workspace.Directory("Content")));
        Assert.True(File.Exists(workspace.File("Blocked")));
        Assert.Null(service.Current);
    }

    [Fact]
    public async Task OpenAsync_RejectsContentLinkEscapeBeforeRecoveringDirectories()
    {
        using var workspace = TempWorkspace.Create();
        using var outside = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var serializer = new WorkspaceManifestSerializer();
        var service = CreateService(recent.File("recent.yaml"));
        var manifestPath = workspace.File("workspace.sailor");
        var linkPath = workspace.Directory("Content");
        await serializer.SaveAsync(
            manifestPath,
            WorkspaceManifest.CreateDefault("Sandbox", "../Sailor", "workspace-id"));
        CreateDirectoryLink(linkPath, outside.Root);

        try
        {
            var result = await service.OpenAsync(manifestPath);

            Assert.False(result.Succeeded);
            Assert.Contains("symbolic link or junction", result.Error, StringComparison.OrdinalIgnoreCase);
            Assert.False(Directory.Exists(workspace.Directory("Cache")));
            Assert.Null(service.Current);
        }
        finally
        {
            if (Directory.Exists(linkPath))
                Directory.Delete(linkPath);
        }
    }

    [Fact]
    public async Task OpenAsync_PublishesPhysicalPathsForContainedDirectoryLinks()
    {
        using var workspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var serializer = new WorkspaceManifestSerializer();
        var service = CreateService(recent.File("recent.yaml"));
        var manifestPath = workspace.File("workspace.sailor");
        var physicalContent = workspace.Directory(Path.Combine("Data", "Content"));
        var contentLink = workspace.Directory("Content");
        Directory.CreateDirectory(physicalContent);
        await serializer.SaveAsync(
            manifestPath,
            WorkspaceManifest.CreateDefault("Sandbox", "../Sailor", "workspace-id"));
        CreateDirectoryLink(contentLink, physicalContent);

        try
        {
            var result = await service.OpenAsync(manifestPath);

            Assert.True(result.Succeeded, result.Error);
            Assert.Equal(
                WorkspacePathPolicy.NormalizePhysicalPath(physicalContent),
                result.Session!.ContentDirectory,
                ignoreCase: OperatingSystem.IsWindows());
        }
        finally
        {
            if (Directory.Exists(contentLink))
                Directory.Delete(contentLink);
        }
    }

    [Fact]
    public async Task SaveAsync_PersistsManifestUpdates()
    {
        using var workspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var service = CreateService(recent.File("recent.yaml"));
        var created = await service.CreateAsync(new WorkspaceCreateRequest("Sandbox", workspace.Root, "../Sailor", "workspace-id"));
        var renamed = created.Session! with
        {
            Manifest = created.Session.Manifest with { Name = "Renamed" }
        };

        var saved = await service.SaveAsync(renamed);
        var reopened = await service.OpenAsync(created.Session.ManifestPath);

        Assert.True(saved.Succeeded, saved.Error);
        Assert.True(reopened.Succeeded, reopened.Error);
        Assert.Equal("Renamed", reopened.Session!.Manifest.Name);
    }

    [Fact]
    public async Task OpenAndSave_DoNotOverwriteGeneratedProjectFiles()
    {
        using var workspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var service = CreateService(recent.File("recent.yaml"));
        var created = await service.CreateAsync(new WorkspaceCreateRequest("Sandbox", workspace.Root, "../Sailor", "workspace-id"));
        var session = Assert.IsType<WorkspaceSession>(created.Session);
        var cmakePath = workspace.File("Generated/CMakeLists.txt");
        await File.WriteAllTextAsync(cmakePath, "user-edited project");

        var saved = await service.SaveAsync(session);
        var reopened = await service.OpenAsync(session.ManifestPath);

        Assert.True(saved.Succeeded, saved.Error);
        Assert.True(reopened.Succeeded, reopened.Error);
        Assert.Equal("user-edited project", await File.ReadAllTextAsync(cmakePath));
    }

    [Fact]
    public async Task SaveAsync_RejectsManifestPathsOutsideWorkspace()
    {
        using var workspace = TempWorkspace.Create();
        using var outside = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var service = CreateService(recent.File("recent.yaml"));
        var created = await service.CreateAsync(new WorkspaceCreateRequest("Sandbox", workspace.Root, "../Sailor", "workspace-id"));
        var unsafeSession = created.Session! with
        {
            ManifestPath = outside.File("workspace.sailor")
        };

        var result = await service.SaveAsync(unsafeSession);

        Assert.False(result.Succeeded);
        Assert.Contains("outside", result.Error, StringComparison.OrdinalIgnoreCase);
        Assert.False(File.Exists(outside.File("workspace.sailor")));
    }

    [Fact]
    public async Task SaveAsync_RejectsManifestContentPathsOutsideWorkspace()
    {
        using var workspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var service = CreateService(recent.File("recent.yaml"));
        var created = await service.CreateAsync(new WorkspaceCreateRequest("Sandbox", workspace.Root, "../Sailor", "workspace-id"));
        var unsafeSession = created.Session! with
        {
            Manifest = created.Session.Manifest with { ContentPath = "../Content" }
        };

        var result = await service.SaveAsync(unsafeSession);

        Assert.False(result.Succeeded);
        Assert.Contains("outside", result.Error, StringComparison.OrdinalIgnoreCase);
    }

    [Theory]
    [InlineData(nameof(WorkspaceManifest.BuildPath))]
    [InlineData(nameof(WorkspaceManifest.LogicOutputPath))]
    public async Task SaveAsync_RejectsLogicProjectPathsOutsideWorkspace(string field)
    {
        using var workspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var service = CreateService(recent.File("recent.yaml"));
        var created = await service.CreateAsync(new WorkspaceCreateRequest("Sandbox", workspace.Root, "../Sailor", "workspace-id"));
        var unsafeManifest = field == nameof(WorkspaceManifest.BuildPath)
            ? created.Session!.Manifest with { BuildPath = "../Build" }
            : created.Session!.Manifest with { LogicOutputPath = "../Binaries" };
        var unsafeSession = created.Session! with { Manifest = unsafeManifest };

        var result = await service.SaveAsync(unsafeSession);

        Assert.False(result.Succeeded);
        Assert.Contains("outside", result.Error, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public async Task CreateAsync_RejectsNonEmptyDirectory()
    {
        using var workspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        await File.WriteAllTextAsync(workspace.File("existing.txt"), "existing");
        var service = CreateService(recent.File("recent.yaml"));

        var result = await service.CreateAsync(new WorkspaceCreateRequest("Sandbox", workspace.Root, "../Sailor"));

        Assert.False(result.Succeeded);
        Assert.Contains("not empty", result.Error, StringComparison.OrdinalIgnoreCase);
        Assert.False(File.Exists(workspace.File(WorkspaceTemplateService.ManifestFileName)));
    }

    [Fact]
    public async Task OpenAsync_ReturnsErrorForMissingManifest()
    {
        using var workspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var service = CreateService(recent.File("recent.yaml"));

        var result = await service.OpenAsync(workspace.File(WorkspaceTemplateService.ManifestFileName));

        Assert.False(result.Succeeded);
        Assert.Contains("not found", result.Error, StringComparison.OrdinalIgnoreCase);
    }

    [Theory]
    [MemberData(nameof(InvalidManifestVersionPayloads))]
    public async Task OpenAsync_InvalidManifestVersionDoesNotMutatePublishedOrCandidateState(string payload)
    {
        using var currentWorkspace = TempWorkspace.Create();
        using var candidateWorkspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var recentPath = recent.File("recent.yaml");
        var service = CreateService(recentPath);
        var current = await service.CreateAsync(new WorkspaceCreateRequest(
            "Current",
            currentWorkspace.Root,
            "../Sailor",
            "current-id"));
        var published = Assert.IsType<WorkspaceSession>(current.Session);
        var recentBefore = await File.ReadAllBytesAsync(recentPath);
        var manifestPath = candidateWorkspace.File("candidate.sailor");
        await File.WriteAllTextAsync(manifestPath, payload);

        var result = await service.OpenAsync(manifestPath);

        Assert.False(result.Succeeded);
        Assert.Same(published, service.Current);
        Assert.Equal(payload, await File.ReadAllTextAsync(manifestPath));
        Assert.Equal(recentBefore, await File.ReadAllBytesAsync(recentPath));
        Assert.False(Directory.Exists(candidateWorkspace.Directory("Content")));
        Assert.False(Directory.Exists(candidateWorkspace.Directory("Cache")));
    }

    [Fact]
    public async Task CreateAsync_RejectsPathsOutsideWorkspace()
    {
        using var workspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var serializer = new WorkspaceManifestSerializer();
        var template = new UnsafeWorkspaceTemplateService(serializer);
        var service = new WorkspaceLifecycleService(serializer, template, new RecentWorkspaceStore(recent.File("recent.yaml")));

        var result = await service.CreateAsync(new WorkspaceCreateRequest("Sandbox", workspace.Root, "../Sailor"));

        Assert.False(result.Succeeded);
        Assert.Contains("outside", result.Error, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public async Task CreateAsync_DoesNotPublishSessionWhenRecentWorkspaceUpdateFails()
    {
        using var currentWorkspace = TempWorkspace.Create();
        using var newWorkspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var recentPath = recent.File("store/recent.yaml");
        var service = CreateService(recentPath);
        var current = await service.CreateAsync(new WorkspaceCreateRequest(
            "Current",
            currentWorkspace.Root,
            "../Sailor",
            "current-id"));
        var published = Assert.IsType<WorkspaceSession>(current.Session);
        BlockRecentStore(recentPath);

        var result = await service.CreateAsync(new WorkspaceCreateRequest(
            "New",
            newWorkspace.Root,
            "../Sailor",
            "new-id"));

        Assert.False(result.Succeeded);
        Assert.Same(published, service.Current);
    }

    [Fact]
    public async Task OpenAsync_DoesNotPublishSessionWhenRecentWorkspaceUpdateFails()
    {
        using var currentWorkspace = TempWorkspace.Create();
        using var openedWorkspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var recentPath = recent.File("store/recent.yaml");
        var service = CreateService(recentPath);
        var serializer = new WorkspaceManifestSerializer();
        var current = await service.CreateAsync(new WorkspaceCreateRequest(
            "Current",
            currentWorkspace.Root,
            "../Sailor",
            "current-id"));
        var published = Assert.IsType<WorkspaceSession>(current.Session);
        var openedManifestPath = openedWorkspace.File("workspace.sailor");
        await serializer.SaveAsync(
            openedManifestPath,
            WorkspaceManifest.CreateDefault("Opened", "../Sailor", "opened-id"));
        BlockRecentStore(recentPath);

        var result = await service.OpenAsync(openedManifestPath);

        Assert.False(result.Succeeded);
        Assert.Same(published, service.Current);
        Assert.False(Directory.Exists(openedWorkspace.Directory("Content")));
        Assert.False(Directory.Exists(openedWorkspace.Directory("Cache")));
    }

    [Fact]
    public async Task SaveAsync_DoesNotPublishSessionWhenRecentWorkspaceUpdateFails()
    {
        using var workspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var recentPath = recent.File("store/recent.yaml");
        var service = CreateService(recentPath);
        var created = await service.CreateAsync(new WorkspaceCreateRequest(
            "Current",
            workspace.Root,
            "../Sailor",
            "current-id"));
        var published = Assert.IsType<WorkspaceSession>(created.Session);
        Directory.Delete(workspace.Directory("Content"), recursive: true);
        Directory.Delete(workspace.Directory("Cache"), recursive: true);
        BlockRecentStore(recentPath);
        var renamed = published with
        {
            Manifest = published.Manifest with { Name = "Renamed" }
        };

        var result = await service.SaveAsync(renamed);

        Assert.False(result.Succeeded);
        Assert.Same(published, service.Current);
        Assert.Equal("Current", service.Current!.Manifest.Name);
        Assert.False(Directory.Exists(workspace.Directory("Content")));
        Assert.False(Directory.Exists(workspace.Directory("Cache")));
    }

    [Fact]
    public async Task SaveAsync_RollsBackRecoveredDirectoriesWhenManifestWriteIsCancelled()
    {
        using var workspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var service = CreateService(recent.File("recent.yaml"));
        var created = await service.CreateAsync(new WorkspaceCreateRequest(
            "Current",
            workspace.Root,
            "../Sailor",
            "current-id"));
        var published = Assert.IsType<WorkspaceSession>(created.Session);
        Directory.Delete(workspace.Directory("Content"), recursive: true);
        Directory.Delete(workspace.Directory("Cache"), recursive: true);
        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();

        var result = await service.SaveAsync(published, cancellation.Token);

        Assert.False(result.Succeeded);
        Assert.Same(published, service.Current);
        Assert.False(Directory.Exists(workspace.Directory("Content")));
        Assert.False(Directory.Exists(workspace.Directory("Cache")));
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task SaveAsync_RejectsMissingOrUnsupportedOnDiskManifestBeforeRecovery(bool writeFutureManifest)
    {
        using var workspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var service = CreateService(recent.File("recent.yaml"));
        var created = await service.CreateAsync(new WorkspaceCreateRequest(
            "Current",
            workspace.Root,
            "../Sailor",
            "current-id"));
        var published = Assert.IsType<WorkspaceSession>(created.Session);
        Directory.Delete(workspace.Directory("Content"), recursive: true);
        Directory.Delete(workspace.Directory("Cache"), recursive: true);

        const string futureManifest = "manifestVersion: 2\nworkspaceId: [not, a, scalar]\n";
        if (writeFutureManifest)
            await File.WriteAllTextAsync(published.ManifestPath, futureManifest);
        else
            File.Delete(published.ManifestPath);

        var result = await service.SaveAsync(published);

        Assert.False(result.Succeeded);
        Assert.Same(published, service.Current);
        Assert.False(Directory.Exists(workspace.Directory("Content")));
        Assert.False(Directory.Exists(workspace.Directory("Cache")));
        if (writeFutureManifest)
            Assert.Equal(futureManifest, await File.ReadAllTextAsync(published.ManifestPath));
        else
            Assert.False(File.Exists(published.ManifestPath));
    }

    [Fact]
    public async Task PrepareOpenAsync_DoesNotPublishOrRecordUntilCommitted()
    {
        using var currentWorkspace = TempWorkspace.Create();
        using var candidateWorkspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var recentPath = recent.File("recent.yaml");
        var service = CreateService(recentPath);
        var serializer = new WorkspaceManifestSerializer();
        var current = await service.CreateAsync(new WorkspaceCreateRequest(
            "Current",
            currentWorkspace.Root,
            "../Sailor",
            "current-id"));
        var published = Assert.IsType<WorkspaceSession>(current.Session);
        var candidateManifest = candidateWorkspace.File("workspace.sailor");
        await serializer.SaveAsync(
            candidateManifest,
            WorkspaceManifest.CreateDefault("Candidate", "../Sailor", "candidate-id"));

        using var prepared = (await service.PrepareOpenAsync(candidateManifest)).Preparation;

        Assert.NotNull(prepared);
        Assert.Same(published, service.Current);
        Assert.Equal("Candidate", prepared.Session.Manifest.Name);
        Assert.True(Directory.Exists(candidateWorkspace.Directory("Content")));
        var entries = await service.LoadRecentAsync();
        Assert.DoesNotContain(entries, entry => entry.WorkspaceId == "candidate-id");
    }

    [Fact]
    public async Task PrepareOpenAsync_DiscardRollsBackRecoveredDirectories()
    {
        using var workspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var serializer = new WorkspaceManifestSerializer();
        var service = CreateService(recent.File("recent.yaml"));
        var manifestPath = workspace.File("workspace.sailor");
        await serializer.SaveAsync(
            manifestPath,
            WorkspaceManifest.CreateDefault("Candidate", "../Sailor", "candidate-id"));
        var result = await service.PrepareOpenAsync(manifestPath);
        var prepared = Assert.IsType<WorkspaceLifecyclePreparation>(result.Preparation);

        prepared.Discard();

        Assert.False(Directory.Exists(workspace.Directory("Content")));
        Assert.False(Directory.Exists(workspace.Directory("Cache")));
        Assert.Null(service.Current);
    }

    [Fact]
    public async Task CommitActivationAsync_PublishesCandidateBeforeRecentStoreFailure()
    {
        using var workspace = TempWorkspace.Create();
        using var recent = TempWorkspace.Create();
        var recentPath = recent.File("store/recent.yaml");
        var serializer = new WorkspaceManifestSerializer();
        var service = CreateService(recentPath);
        var manifestPath = workspace.File("workspace.sailor");
        await serializer.SaveAsync(
            manifestPath,
            WorkspaceManifest.CreateDefault("Candidate", "../Sailor", "candidate-id"));
        var result = await service.PrepareOpenAsync(manifestPath);
        var prepared = Assert.IsType<WorkspaceLifecyclePreparation>(result.Preparation);
        BlockRecentStore(recentPath);

        await Assert.ThrowsAnyAsync<Exception>(() => service.CommitActivationAsync(prepared));

        Assert.Same(prepared.Session, service.Current);
        Assert.True(Directory.Exists(workspace.Directory("Content")));
        Assert.True(Directory.Exists(workspace.Directory("Cache")));
    }

    [Fact]
    public async Task RecentWorkspaceStore_RoundTripsNewestFirstAndDeduplicates()
    {
        using var workspace = TempWorkspace.Create();
        var now = new DateTimeOffset(2026, 6, 25, 12, 0, 0, TimeSpan.Zero);
        var store = new RecentWorkspaceStore(workspace.File("recent.yaml"), () => now);
        var manifest = WorkspaceManifest.CreateDefault("Sandbox", "../Sailor", "workspace-id");
        var first = new WorkspaceSession(
            workspace.Root,
            workspace.File("first.sailor"),
            manifest,
            workspace.Directory("Content"),
            workspace.Directory("Source"),
            workspace.Directory("Generated"),
            workspace.Directory("Cache"))
        {
            BuildDirectory = workspace.Directory("Cache/Build"),
            LogicOutputDirectory = workspace.Directory("Binaries")
        };
        var second = first with
        {
            ManifestPath = workspace.File("second.sailor"),
            Manifest = manifest with { WorkspaceId = "workspace-id-2", Name = "Second" }
        };

        await store.RecordAsync(first);
        now = now.AddMinutes(1);
        await store.RecordAsync(second);
        now = now.AddMinutes(1);
        await store.RecordAsync(first with { Manifest = manifest with { Name = "Sandbox Updated" } });

        var entries = await store.LoadAsync();

        Assert.Equal(2, entries.Count);
        Assert.Equal("Sandbox Updated", entries[0].Name);
        Assert.Equal(Path.GetFullPath(first.ManifestPath), entries[0].ManifestPath);
        Assert.Equal("Second", entries[1].Name);
    }

    [Fact]
    public async Task RecentWorkspaceStore_ReturnsEmptyListForCorruptedYaml()
    {
        using var workspace = TempWorkspace.Create();
        var path = workspace.File("recent.yaml");
        await File.WriteAllTextAsync(path, "not: [valid");
        var store = new RecentWorkspaceStore(path);

        var entries = await store.LoadAsync();

        Assert.Empty(entries);
    }

    static WorkspaceLifecycleService CreateService(string recentPath)
    {
        var serializer = new WorkspaceManifestSerializer();
        var template = new WorkspaceTemplateService(serializer);
        var recentStore = new RecentWorkspaceStore(recentPath);
        return new WorkspaceLifecycleService(serializer, template, recentStore);
    }

    public static TheoryData<string> InvalidManifestVersionPayloads => new()
    {
        "workspaceId: candidate-id\n",
        "manifestVersion: 0\n",
        "manifestVersion: 2\nworkspaceId: [not, a, scalar]\n",
        "manifestVersion: 1\nmanifestVersion: 1\n",
        "manifestVersion: [1]\n"
    };

    static string Physical(string path) => WorkspacePathPolicy.NormalizePhysicalPath(path);

    static void BlockRecentStore(string recentPath)
    {
        if (File.Exists(recentPath))
            File.Delete(recentPath);

        var directory = Path.GetDirectoryName(recentPath)
            ?? throw new InvalidOperationException("Recent workspace path has no directory.");
        if (Directory.Exists(directory))
            Directory.Delete(directory, recursive: true);

        File.WriteAllText(directory, "blocks recent-workspace directory creation");
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

    sealed class UnsafeWorkspaceTemplateService(WorkspaceManifestSerializer serializer) : WorkspaceTemplateService(serializer)
    {
        public override Task<WorkspaceSession> CreateAsync(WorkspaceCreateRequest request, CancellationToken cancellationToken = default)
        {
            var manifest = WorkspaceManifest.CreateDefault(request.Name, request.EnginePath, request.WorkspaceId) with
            {
                ContentPath = "../Content"
            };

            return Task.FromResult(CreateSession(request.WorkspaceDirectory, manifest));
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
            => new(Path.Combine(Path.GetTempPath(), $"sailor-workspace-{Guid.NewGuid():N}"));

        public string File(string relativePath) => Path.Combine(Root, relativePath);

        public string Directory(string relativePath) => Path.Combine(Root, relativePath);

        public void Dispose()
        {
            if (System.IO.Directory.Exists(Root))
                System.IO.Directory.Delete(Root, recursive: true);
        }
    }
}
