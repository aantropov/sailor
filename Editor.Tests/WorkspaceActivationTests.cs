using SailorEditor.Services;
using SailorEditor.Workspace;

namespace SailorEditor.Editor.Tests;

public sealed class WorkspaceActivationTests
{
    [Fact]
    public async Task ActivateAsync_UsesExactOrderingAndCandidateLaunchContext()
    {
        var actions = new RecordingActivationOperations();
        var coordinator = new WorkspaceActivationCoordinator(actions);
        var candidate = CreateCandidate("B");
        var phases = new List<WorkspaceActivationPhase>();
        coordinator.StateChanged += (_, state) => phases.Add(state.Phase);

        var result = await coordinator.ActivateAsync(new WorkspaceActivationRequest(
            "B",
            cancellationToken =>
            {
                cancellationToken.ThrowIfCancellationRequested();
                actions.Events.Add("preflight:B");
                return Task.FromResult(candidate);
            }));

        Assert.True(result.Succeeded, result.Error);
        Assert.Equal(
            ["preflight:B", "stop", "clear:1", "commit:B", "start:B:1"],
            actions.Events);
        Assert.Equal(
            [
                WorkspaceActivationPhase.Preflighting,
                WorkspaceActivationPhase.Stopping,
                WorkspaceActivationPhase.Clearing,
                WorkspaceActivationPhase.Committing,
                WorkspaceActivationPhase.Starting,
                WorkspaceActivationPhase.Ready
            ],
            phases);
        Assert.Same(candidate.LaunchContext, actions.StartedContext);
        Assert.Equal(candidate.LaunchContext.WorkspaceRoot, actions.StartedContext!.WorkspaceRoot);
        Assert.Equal(candidate.LaunchContext.WorkspaceManifestPath, actions.StartedContext.WorkspaceManifestPath);
    }

    [Fact]
    public async Task ActivateAsync_PreflightFailureLeavesCurrentCandidateUntouched()
    {
        var actions = new RecordingActivationOperations();
        var coordinator = new WorkspaceActivationCoordinator(actions);
        var active = CreateCandidate("A");
        Assert.True((await coordinator.ActivateAsync(Request(active))).Succeeded);
        actions.Events.Clear();

        var failed = await coordinator.ActivateAsync(new WorkspaceActivationRequest(
            "B",
            _ => throw new InvalidOperationException("candidate invalid")));

        Assert.False(failed.Succeeded);
        Assert.False(failed.RequiresRepair);
        Assert.Equal(WorkspaceActivationPhase.Ready, coordinator.State.Phase);
        Assert.Same(active, coordinator.State.Candidate);
        Assert.Equal(1, coordinator.State.Generation);
        Assert.Empty(actions.Events);
    }

    [Fact]
    public async Task ActivateAsync_SerializesRapidRequestsInFifoOrder()
    {
        var actions = new RecordingActivationOperations();
        var coordinator = new WorkspaceActivationCoordinator(actions);
        var releaseB = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var bEntered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var cPreflighted = false;

        var activateB = coordinator.ActivateAsync(new WorkspaceActivationRequest(
            "B",
            async _ =>
            {
                actions.Events.Add("preflight:B");
                bEntered.SetResult();
                await releaseB.Task;
                return CreateCandidate("B");
            }));
        await bEntered.Task;

        var activateC = coordinator.ActivateAsync(new WorkspaceActivationRequest(
            "C",
            _ =>
            {
                cPreflighted = true;
                actions.Events.Add("preflight:C");
                return Task.FromResult(CreateCandidate("C"));
            }));

        await Task.Yield();
        Assert.False(cPreflighted);
        releaseB.SetResult();

        var results = await Task.WhenAll(activateB, activateC);

        Assert.All(results, result => Assert.True(result.Succeeded, result.Error));
        Assert.Equal(
            [
                "preflight:B", "stop", "clear:1", "commit:B", "start:B:1",
                "preflight:C", "stop", "clear:2", "commit:C", "start:C:2"
            ],
            actions.Events);
        Assert.Equal("C", coordinator.State.Candidate!.Session.Manifest.Name);
        Assert.Equal(2, coordinator.State.Generation);
    }

    [Fact]
    public async Task ActivateAsync_SwitchingAtoBtoAUsesFreshGenerations()
    {
        var actions = new RecordingActivationOperations();
        var coordinator = new WorkspaceActivationCoordinator(actions);

        Assert.True((await coordinator.ActivateAsync(Request(CreateCandidate("A")))).Succeeded);
        Assert.True((await coordinator.ActivateAsync(Request(CreateCandidate("B")))).Succeeded);
        var result = await coordinator.ActivateAsync(Request(CreateCandidate("A")));

        Assert.True(result.Succeeded, result.Error);
        Assert.Equal("A", result.Candidate!.Session.Manifest.Name);
        Assert.Equal(3, result.State.Generation);
        Assert.Equal(3, actions.Events.Count(entry => entry == "stop"));
        Assert.Equal(3, actions.Events.Count(entry => entry.StartsWith("clear:", StringComparison.Ordinal)));
    }

    [Fact]
    public async Task RunSerializedAsync_WaitsForPriorActivation()
    {
        var stopEntered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseStop = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var actions = new RecordingActivationOperations
        {
            Stop = async _ =>
            {
                stopEntered.TrySetResult();
                await releaseStop.Task;
            }
        };
        var coordinator = new WorkspaceActivationCoordinator(actions);
        var activation = coordinator.ActivateAsync(Request(CreateCandidate("B")));
        await stopEntered.Task;

        var serializedRan = false;
        var serialized = coordinator.RunSerializedAsync(_ =>
        {
            serializedRan = true;
            actions.Events.Add("serialized");
            return Task.CompletedTask;
        });
        await Task.Yield();
        Assert.False(serializedRan);

        releaseStop.TrySetResult();
        await Task.WhenAll(activation, serialized);

        Assert.Equal("serialized", actions.Events.Last());
    }

    [Fact]
    public async Task ActivateAsync_CancellationDuringPreflightDoesNotCrossStopBoundary()
    {
        var actions = new RecordingActivationOperations();
        var coordinator = new WorkspaceActivationCoordinator(actions);
        using var cancellation = new CancellationTokenSource();
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var activation = coordinator.ActivateAsync(new WorkspaceActivationRequest(
            "B",
            async cancellationToken =>
            {
                entered.SetResult();
                await Task.Delay(Timeout.InfiniteTimeSpan, cancellationToken);
                return CreateCandidate("B");
            }), cancellation.Token);

        await entered.Task;
        cancellation.Cancel();
        var result = await activation;

        Assert.True(result.WasCancelled);
        Assert.Equal(WorkspaceActivationPhase.Idle, result.State.Phase);
        Assert.Empty(actions.Events);
    }

    [Fact]
    public async Task ActivateAsync_CancellationAfterStopBeginsCompletesTransaction()
    {
        using var cancellation = new CancellationTokenSource();
        var stopEntered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseStop = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var actions = new RecordingActivationOperations
        {
            Stop = async cancellationToken =>
            {
                Assert.False(cancellationToken.CanBeCanceled);
                stopEntered.SetResult();
                await releaseStop.Task;
            }
        };
        var coordinator = new WorkspaceActivationCoordinator(actions);
        var activation = coordinator.ActivateAsync(Request(CreateCandidate("B")), cancellation.Token);

        await stopEntered.Task;
        cancellation.Cancel();
        releaseStop.SetResult();
        var result = await activation;

        Assert.True(result.Succeeded, result.Error);
        Assert.Equal(["stop", "clear:1", "commit:B", "start:B:1"], actions.Events);
    }

    [Fact]
    public async Task ActivateAsync_PostStopFailureCompletesClearAndCommitThenEntersRepair()
    {
        var actions = new RecordingActivationOperations
        {
            Stop = _ => throw new InvalidOperationException("shutdown failed")
        };
        var coordinator = new WorkspaceActivationCoordinator(actions);

        var result = await coordinator.ActivateAsync(Request(CreateCandidate("B")));

        Assert.False(result.Succeeded);
        Assert.True(result.RequiresRepair);
        Assert.Contains("shutdown failed", result.Error, StringComparison.Ordinal);
        Assert.Equal(["stop", "clear:1", "commit:B"], actions.Events);
        Assert.DoesNotContain(actions.Events, item => item.StartsWith("start:", StringComparison.Ordinal));
        Assert.Equal("B", coordinator.State.Candidate!.Session.Manifest.Name);
    }

    [Fact]
    public async Task ReportRuntimeFailure_MovesReadyWorkspaceToRepair()
    {
        var coordinator = new WorkspaceActivationCoordinator(new RecordingActivationOperations());
        var candidate = CreateCandidate("A");
        Assert.True((await coordinator.ActivateAsync(Request(candidate))).Succeeded);

        var reported = coordinator.ReportRuntimeFailure("native main loop failed");

        Assert.True(reported);
        Assert.Equal(WorkspaceActivationPhase.Repair, coordinator.State.Phase);
        Assert.Equal("native main loop failed", coordinator.State.Error);
        Assert.Same(candidate, coordinator.State.Candidate);
    }

    [Fact]
    public async Task ReportRuntimeFailure_DuringFailedPreflightRestoresRepairNotReady()
    {
        var coordinator = new WorkspaceActivationCoordinator(new RecordingActivationOperations());
        Assert.True((await coordinator.ActivateAsync(Request(CreateCandidate("A")))).Succeeded);
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var activation = coordinator.ActivateAsync(new WorkspaceActivationRequest(
            "B",
            async _ =>
            {
                entered.TrySetResult();
                await release.Task;
                throw new InvalidOperationException("candidate invalid");
            }));

        await entered.Task;
        Assert.True(coordinator.ReportRuntimeFailure("old runtime failed"));
        release.TrySetResult();
        var result = await activation;

        Assert.True(result.RequiresRepair);
        Assert.Equal("old runtime failed", result.Error);
        Assert.Equal(WorkspaceActivationPhase.Repair, coordinator.State.Phase);
    }

    [Fact]
    public async Task ReportRuntimeFailure_DuringStartingPreventsReadyPublication()
    {
        var coordinator = new WorkspaceActivationCoordinator(new RecordingActivationOperations());
        coordinator.StateChanged += (_, state) =>
        {
            if (state.Phase == WorkspaceActivationPhase.Starting)
                coordinator.ReportRuntimeFailure("runtime exited during startup");
        };

        var result = await coordinator.ActivateAsync(Request(CreateCandidate("B")));

        Assert.False(result.Succeeded);
        Assert.True(result.RequiresRepair);
        Assert.Contains("runtime exited during startup", result.Error, StringComparison.Ordinal);
        Assert.Equal(WorkspaceActivationPhase.Repair, coordinator.State.Phase);
    }

    static WorkspaceActivationRequest Request(WorkspaceActivationCandidate candidate)
        => new(candidate.Session.Manifest.Name, _ => Task.FromResult(candidate));

    static WorkspaceActivationCandidate CreateCandidate(string name)
    {
        var root = Path.Combine(Path.GetTempPath(), "SailorActivation", name);
        var manifest = WorkspaceManifest.CreateDefault(name, "../Sailor", $"workspace-{name}");
        var session = new WorkspaceSession(
            root,
            Path.Combine(root, $"{name}.sailor"),
            manifest,
            Path.Combine(root, "Content"),
            Path.Combine(root, "Source"),
            Path.Combine(root, "Generated"),
            Path.Combine(root, "Cache"));
        var launchContext = new EngineLaunchContext(
            session.WorkspaceRoot,
            session.ManifestPath,
            session.ContentDirectory,
            session.CacheDirectory,
            session.Manifest.WorkspaceId);
        return new WorkspaceActivationCandidate(new WorkspaceLifecyclePreparation(session), launchContext);
    }

    sealed class RecordingActivationOperations : IWorkspaceActivationOperations
    {
        public List<string> Events { get; } = [];
        public Func<CancellationToken, Task>? Stop { get; init; }
        public EngineLaunchContext? StartedContext { get; private set; }

        public async Task StopAsync(CancellationToken cancellationToken)
        {
            Events.Add("stop");
            if (Stop is not null)
                await Stop(cancellationToken);
        }

        public Task ClearAsync(long generation, CancellationToken cancellationToken)
        {
            Assert.False(cancellationToken.CanBeCanceled);
            Events.Add($"clear:{generation}");
            return Task.CompletedTask;
        }

        public Task CommitAsync(WorkspaceActivationCandidate candidate, CancellationToken cancellationToken)
        {
            Assert.False(cancellationToken.CanBeCanceled);
            Events.Add($"commit:{candidate.Session.Manifest.Name}");
            return Task.CompletedTask;
        }

        public Task StartAsync(
            EngineLaunchContext launchContext,
            long generation,
            CancellationToken cancellationToken)
        {
            Assert.False(cancellationToken.CanBeCanceled);
            StartedContext = launchContext;
            Events.Add($"start:{launchContext.WorkspaceRoot.Split(Path.DirectorySeparatorChar).Last()}:{generation}");
            return Task.CompletedTask;
        }
    }
}
