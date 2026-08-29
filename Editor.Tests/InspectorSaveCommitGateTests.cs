using SailorEditor.ViewModels;
using SailorEditor.Workflow;

namespace Editor.Tests;

public sealed class InspectorSaveCommitGateTests
{
    [Fact]
    public async Task InFlightPointLightEdit_IsCommittedBeforeSaveContinues()
    {
        var commitStarted = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var releaseCommit = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var editableHasPending = true;
        var editable = new TestEditable(async cancellationToken =>
        {
            commitStarted.SetResult();
            await releaseCommit.Task.WaitAsync(cancellationToken);
            editableHasPending = false;
            return true;
        });
        editable.HasPending = () => editableHasPending;

        var saveCanContinue = InspectorSaveCommitGate.CommitSelectedAsync(
            editable);

        await commitStarted.Task;
        Assert.False(saveCanContinue.IsCompleted);

        releaseCommit.SetResult();

        Assert.True(await saveCanContinue);
        Assert.Equal(1, editable.CommitCount);
    }

    [Fact]
    public async Task EditArrivingDuringCommit_IsRetriedBeforeSaveContinues()
    {
        var hasPending = true;
        var commitAttempt = 0;
        var editable = new TestEditable(_ =>
        {
            ++commitAttempt;
            if (commitAttempt >= 2)
            {
                hasPending = false;
            }
            return Task.FromResult(true);
        })
        {
            HasPending = () => hasPending
        };

        Assert.True(await InspectorSaveCommitGate.CommitSelectedAsync(
            editable));
        Assert.Equal(2, editable.CommitCount);
    }

    [Fact]
    public async Task FailedPendingEdit_BlocksSaveAndBake()
    {
        var editable = new TestEditable(_ => Task.FromResult(false))
        {
            HasPending = () => true,
            HasInFlight = () => false
        };

        Assert.False(await InspectorSaveCommitGate.CommitSelectedAsync(
            editable));
        Assert.Equal(1, editable.CommitCount);
    }

    [Fact]
    public async Task RedundantCommitWhileAutoCommitIsInFlight_IsRetried()
    {
        var hasPending = true;
        var hasInFlight = true;
        var editableCommitCount = 0;
        var editable = new TestEditable(_ =>
        {
            if (editableCommitCount == 1)
            {
                return Task.FromResult(false);
            }

            hasPending = false;
            hasInFlight = false;
            return Task.FromResult(false);
        })
        {
            HasPending = () => hasPending,
            HasInFlight = () => hasInFlight
        };
        editable.BeforeCommit = () => ++editableCommitCount;

        Assert.True(await InspectorSaveCommitGate.CommitSelectedAsync(
            editable));
        Assert.Equal(2, editable.CommitCount);
    }

    [Fact]
    public async Task UnrelatedSelection_DoesNotBlockSave()
    {
        Assert.True(await InspectorSaveCommitGate.CommitSelectedAsync(
            new object()));
    }

    sealed class TestEditable(
        Func<CancellationToken, Task<bool>> commit) : IInspectorEditable
    {
        public Func<bool> HasPending { get; set; } = () => true;
        public Func<bool> HasInFlight { get; set; } = () => false;
        public Action BeforeCommit { get; set; } = () => { };
        public int CommitCount { get; private set; }

        public bool HasPendingInspectorChanges => HasPending();
        public bool HasInFlightInspectorCommit => HasInFlight();

        public async Task<bool> CommitInspectorChangesAsync(
            CancellationToken cancellationToken = default)
        {
            ++CommitCount;
            BeforeCommit();
            return await commit(cancellationToken);
        }
    }
}
