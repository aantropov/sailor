using SailorEditor.Scene;
using SailorEditor.Workflow;

namespace Editor.Tests;

public sealed class InspectorPendingEditCoordinatorTests
{
    [Fact]
    public void PendingInspectorCommit_InvalidatesOlderViewportTransformBeforeProjection()
    {
        var coordinator = new InspectorPendingEditCoordinator();
        const ulong eventRevision = 7;
        var currentManagedMutationRevision = eventRevision;
        var commitCount = 0;
        coordinator.CommitPendingChangesRequested += () =>
        {
            ++commitCount;
            ++currentManagedMutationRevision;
            return true;
        };

        Assert.True(EditorViewportMutationOrder.IsCurrent(eventRevision, currentManagedMutationRevision));

        Assert.True(coordinator.CommitPendingChanges());

        Assert.Equal(1, commitCount);
        Assert.False(EditorViewportMutationOrder.IsCurrent(eventRevision, currentManagedMutationRevision));
    }

    [Fact]
    public void FailedPendingInspectorCommit_BlocksViewportProjection()
    {
        var coordinator = new InspectorPendingEditCoordinator();
        var laterHandlerCalled = false;
        coordinator.CommitPendingChangesRequested += () => false;
        coordinator.CommitPendingChangesRequested += () =>
        {
            laterHandlerCalled = true;
            return true;
        };

        Assert.False(coordinator.CommitPendingChanges());
        Assert.False(laterHandlerCalled);
    }
}
