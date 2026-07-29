using SailorEditor.Scene;
using SailorEditor.Workflow;

namespace Editor.Tests;

public sealed class InspectorPendingEditCoordinatorTests
{
    [Fact]
    public async Task PendingInspectorCommit_InvalidatesOlderViewportTransformBeforeProjection()
    {
        var coordinator = new InspectorPendingEditCoordinator();
        const ulong eventRevision = 7;
        var currentManagedMutationRevision = eventRevision;
        var commitCount = 0;
        coordinator.CommitPendingChangesRequested += _ =>
        {
            ++commitCount;
            ++currentManagedMutationRevision;
            return Task.FromResult(true);
        };

        Assert.True(EditorViewportMutationOrder.IsCurrent(eventRevision, currentManagedMutationRevision));

        Assert.True(await coordinator.CommitPendingChangesAsync());

        Assert.Equal(1, commitCount);
        Assert.False(EditorViewportMutationOrder.IsCurrent(eventRevision, currentManagedMutationRevision));
    }

    [Fact]
    public async Task FailedPendingInspectorCommit_BlocksViewportProjection()
    {
        var coordinator = new InspectorPendingEditCoordinator();
        var laterHandlerCalled = false;
        coordinator.CommitPendingChangesRequested += _ =>
            Task.FromResult(false);
        coordinator.CommitPendingChangesRequested += _ =>
        {
            laterHandlerCalled = true;
            return Task.FromResult(true);
        };

        Assert.False(await coordinator.CommitPendingChangesAsync());
        Assert.False(laterHandlerCalled);
    }

    [Fact]
    public async Task InFlightReDirty_RetainsBindingUntilRetryCommitsLatestEdit()
    {
        var commitStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseCommit = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var hasPendingChanges = true;
        var commitAttempts = 0;
        var bindingReplaceCount = 0;

        async Task<InspectorRefreshCommitOutcome> CommitPendingChanges(
            CancellationToken cancellationToken)
        {
            ++commitAttempts;
            hasPendingChanges = false;
            if (commitAttempts == 1)
            {
                commitStarted.SetResult();
                await releaseCommit.Task.WaitAsync(cancellationToken);
            }

            return InspectorRefreshCommitOutcome.FromCompletedCommit(
                commitSucceeded: true,
                hasPendingChanges: hasPendingChanges);
        }

        var refresh = InspectorRefreshCommitGate.TryApplyAsync(
            CommitPendingChanges,
            () => ++bindingReplaceCount);

        await commitStarted.Task;
        hasPendingChanges = true;

        Assert.Equal(0, bindingReplaceCount);

        releaseCommit.SetResult();

        Assert.True(await refresh);
        Assert.Equal(2, commitAttempts);
        Assert.Equal(1, bindingReplaceCount);
    }

    [Fact]
    public async Task FailedCommit_DoesNotReplaceBindingOrSpinRetry()
    {
        var commitAttempts = 0;
        var bindingReplaceCount = 0;

        var refreshed = await InspectorRefreshCommitGate.TryApplyAsync(
            _ =>
            {
                ++commitAttempts;
                return Task.FromResult(
                    InspectorRefreshCommitOutcome.FromCompletedCommit(
                        commitSucceeded: false,
                        hasPendingChanges: true));
            },
            () => ++bindingReplaceCount);

        Assert.False(refreshed);
        Assert.Equal(1, commitAttempts);
        Assert.Equal(0, bindingReplaceCount);
    }

    [Fact]
    public async Task ResetGenerationChange_DiscardsInFlightCommitAndAppliesResetOnce()
    {
        var commitStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseCommit = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var resetGeneration = 0L;
        var commitAttempts = 0;
        var bindingReplaceCount = 0;

        async Task<InspectorRefreshCommitOutcome> CommitPendingChanges(
            CancellationToken cancellationToken)
        {
            ++commitAttempts;
            commitStarted.SetResult();
            await releaseCommit.Task.WaitAsync(cancellationToken);
            return new InspectorRefreshCommitOutcome(
                CanRefresh: false,
                ShouldRetry: true);
        }

        var refreshResetGeneration = resetGeneration;
        var refresh = InspectorRefreshCommitGate.TryApplyAsync(
            CommitPendingChanges,
            () => ++bindingReplaceCount,
            () => resetGeneration != refreshResetGeneration);

        await commitStarted.Task;
        ++resetGeneration;
        releaseCommit.SetResult();

        Assert.True(await refresh);
        Assert.Equal(1, commitAttempts);
        Assert.Equal(1, bindingReplaceCount);
    }
}
