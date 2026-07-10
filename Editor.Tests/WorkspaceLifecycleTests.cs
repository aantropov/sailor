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
        Assert.Equal("workspace-id", result.Session.Manifest.WorkspaceId);
        Assert.Equal(workspace.Directory("Cache/Build"), result.Session.BuildDirectory);
        Assert.Equal(workspace.Directory("Binaries"), result.Session.LogicOutputDirectory);
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
        Assert.Equal(workspace.Directory("Content"), reopened.Session.ContentDirectory);
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
        Assert.Equal(Path.GetFullPath(manifestPath), result.Session!.ManifestPath);
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
        Assert.Equal(Path.GetFullPath(manifestPath), result.Session!.ManifestPath);
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
        await serializer.SaveAsync(manifestPath, manifest);

        var result = await service.OpenAsync(manifestPath);

        Assert.True(result.Succeeded, result.Error);
        Assert.Equal(Path.GetFullPath(workspace.Directory("ProjectContent")), result.Session!.ContentDirectory);
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
