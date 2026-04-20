using SailorEditor.Utility;
using Xunit;

namespace Editor.Tests;

public sealed class EditorPerfTests
{
    [Fact]
    public void ScopePublishesElapsedSample()
    {
        EditorPerfSample? sample = null;

        using (new EditorPerfScope("Test.Scope", value => sample = value))
        {
            Thread.Sleep(5);
        }

        Assert.True(sample.HasValue);
        Assert.Equal("Test.Scope", sample.Value.Name);
        Assert.True(sample.Value.Duration >= TimeSpan.Zero);
    }

    [Fact]
    public void RingBufferedBatcherKeepsLatestHistoryWithinCapacity()
    {
        var batcher = new RingBufferedBatcher<int>(3);

        batcher.EnqueueRange([1, 2]);
        batcher.EnqueueRange([3, 4]);

        Assert.Equal([2, 3, 4], batcher.Snapshot());
    }

    [Fact]
    public void RingBufferedBatcherDrainsPendingBatchWithoutLosingHistory()
    {
        var batcher = new RingBufferedBatcher<string>(4);

        batcher.EnqueueRange(["a", "b"]);
        Assert.True(batcher.HasPending);

        Assert.Equal(["a", "b"], batcher.DrainPending());
        Assert.False(batcher.HasPending);
        Assert.Equal(["a", "b"], batcher.Snapshot());

        batcher.EnqueueRange(["c"]);
        Assert.Equal(["c"], batcher.DrainPending());
        Assert.Equal(["a", "b", "c"], batcher.Snapshot());
    }
}
