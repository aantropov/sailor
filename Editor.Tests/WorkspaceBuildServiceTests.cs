using SailorEditor.Workspace;

namespace SailorEditor.Tests;

public sealed class WorkspaceBuildServiceTests : IDisposable
{
    readonly string root = Directory.CreateTempSubdirectory("sailor-build-tests-").FullName;
    readonly FakeRunner runner = new();

    WorkspaceBuildService CreateService()
    {
        var serializer = new WorkspaceManifestSerializer();
        var lifecycle = new WorkspaceLifecycleService(serializer,
            new WorkspaceTemplateService(serializer),
            new RecentWorkspaceStore(Path.Combine(root, "recent.yaml")));
        return new WorkspaceBuildService(lifecycle, runner);
    }

    WorkspaceSession Session => new(root, Path.Combine(root, "Game.sailor"),
        WorkspaceManifest.CreateDefault("Game", Path.Combine(root, "Engine")),
        Path.Combine(root, "Content"), Path.Combine(root, "Source"),
        Path.Combine(root, "Generated"), Path.Combine(root, "Cache"))
    {
        BuildDirectory = Path.Combine(root, "Cache", "Build"),
        LogicOutputDirectory = Path.Combine(root, "Binaries"),
    };

    [Fact]
    public async Task Build_RequiresAnActiveWorkspace()
    {
        var result = await CreateService().BuildAsync("Release", configure: true);
        Assert.False(result.Succeeded);
        Assert.Empty(runner.Invocations);
    }

    [Fact]
    public async Task Build_StopsAfterConfigureFailureAndKeepsCompilerOutput()
    {
        runner.ExitCode = 23;
        var result = await CreateService().BuildAsync(Session, "Release", configure: true);
        Assert.False(result.Succeeded);
        Assert.Equal(23, result.ExitCode);
        Assert.Single(runner.Invocations);
        Assert.Contains("compiler diagnostic", result.Output);
    }

    [Fact]
    public async Task Configure_DoesNotBuildOrOverwriteTheGeneratedProject()
    {
        Directory.CreateDirectory(Session.GeneratedProjectDirectory);
        var project = Path.Combine(Session.GeneratedProjectDirectory, "CMakeLists.txt");
        const string customProject = "project(UserOwnedProject)";
        await File.WriteAllTextAsync(project, customProject);
        var result = await CreateService().ConfigureAsync(Session, "Release");
        Assert.True(result.Succeeded, result.Error);
        Assert.Single(runner.Invocations);
        Assert.DoesNotContain("--build", runner.Invocations[0].Arguments);
        Assert.Equal(customProject, await File.ReadAllTextAsync(project));
    }

    [Fact]
    public async Task Build_SucceedsAgainAfterCancelledInvocation()
    {
        var service = CreateService();
        using var cancellation = new CancellationTokenSource();
        runner.BeforeRun = () => cancellation.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
            service.BuildAsync(Session, "Release", false, cancellation.Token));
        runner.BeforeRun = null;
        var result = await service.BuildAsync(Session, "Release", false);
        Assert.True(result.Succeeded, result.Error);
    }

    [Fact]
    public async Task Build_RejectsStaleGeneratedStateWithoutRunningCommands()
    {
        var session = Session with
        {
            GeneratedProjectState = new(WorkspaceGeneratedProjectStateStatus.Untracked,
                "Review generated project state first.", []),
        };
        var service = CreateService();
        Assert.False((await service.BuildAsync(session, "Release", true)).Succeeded);
        Assert.False((await service.ConfigureAsync(session, "Release")).Succeeded);
        Assert.Empty(runner.Invocations);
    }

    public void Dispose() => Directory.Delete(root, recursive: true);

    sealed class FakeRunner : IWorkspaceProcessRunner
    {
        public readonly List<WorkspaceProcessInvocation> Invocations = [];
        public int ExitCode;
        public Action? BeforeRun;

        public Task<WorkspaceProcessResult> RunAsync(WorkspaceProcessInvocation invocation,
            CancellationToken cancellationToken = default)
        {
            Invocations.Add(invocation);
            BeforeRun?.Invoke();
            cancellationToken.ThrowIfCancellationRequested();
            return Task.FromResult(new WorkspaceProcessResult(ExitCode,
                "compiler diagnostic", TimeSpan.FromMilliseconds(1)));
        }
    }
}
