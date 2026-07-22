using SailorEditor.Services;

namespace SailorEditor.Tests;

public sealed class WorldSnapshotPublicationGateTests
{
    [Fact]
    public void FreshSynchronousSnapshot_PreventsOlderQueuedSnapshotFromPublishing()
    {
        var gate = new WorldSnapshotPublicationGate();
        var olderQueuedSnapshot = gate.ReserveSequence();
        var freshSynchronousSnapshot = gate.ReserveSequence();

        Assert.True(gate.TryAdvance(freshSynchronousSnapshot));
        Assert.False(gate.TryAdvance(olderQueuedSnapshot));
    }

    [Fact]
    public void LocalMutationBarrier_PreventsOlderQueuedSnapshotFromPublishing()
    {
        var gate = new WorldSnapshotPublicationGate();
        var olderQueuedSnapshot = gate.ReserveSequence();
        var localMutationBarrier = gate.ReserveSequence();

        Assert.True(gate.TryAdvance(localMutationBarrier));
        Assert.False(gate.TryAdvance(olderQueuedSnapshot));
    }
}
