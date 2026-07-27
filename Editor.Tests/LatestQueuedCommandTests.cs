using SailorEditor.Services;

namespace Editor.Tests;

public sealed class LatestQueuedCommandTests
{
    [Fact]
    public void Enqueue_CoalescesPendingValuesToTheLatest()
    {
        var queue = new LatestQueuedCommand<int>();
        var scheduled = new List<Func<bool>>();
        var executed = new List<int>();

        Assert.True(queue.Enqueue(
            1,
            action =>
            {
                scheduled.Add(action);
                return true;
            },
            value =>
            {
                executed.Add(value);
                return true;
            }));
        Assert.True(queue.Enqueue(
            2,
            action =>
            {
                scheduled.Add(action);
                return true;
            },
            value =>
            {
                executed.Add(value);
                return true;
            }));

        Assert.Single(scheduled);
        Assert.True(scheduled[0]());
        Assert.Equal([2], executed);
    }

    [Fact]
    public void Enqueue_PreservesValueArrivingDuringExecution()
    {
        var queue = new LatestQueuedCommand<int>();
        var scheduled = new List<Func<bool>>();
        var executed = new List<int>();
        Func<Func<bool>, bool> schedule = action =>
        {
            scheduled.Add(action);
            return true;
        };
        bool Execute(int value)
        {
            executed.Add(value);
            if (value == 1)
            {
                Assert.True(queue.Enqueue(2, schedule, Execute));
            }
            return true;
        }

        Assert.True(queue.Enqueue(1, schedule, Execute));
        Assert.True(scheduled[0]());
        Assert.Equal(2, scheduled.Count);
        Assert.True(scheduled[1]());
        Assert.Equal([1, 2], executed);
    }

    [Fact]
    public void Enqueue_ValueArrivingDuringExecutionPrecedesLaterEdgeCommand()
    {
        var queue = new LatestQueuedCommand<int>();
        var scheduled = new List<Func<bool>>();
        var executed = new List<string>();
        bool Schedule(Func<bool> action)
        {
            scheduled.Add(action);
            return true;
        }
        bool Execute(int value)
        {
            executed.Add($"value:{value}");
            if (value == 1)
            {
                Assert.True(queue.Enqueue(2, Schedule, Execute));
                Assert.True(Schedule(() =>
                {
                    executed.Add("edge");
                    return true;
                }));
            }
            return true;
        }

        Assert.True(queue.Enqueue(1, Schedule, Execute));
        Assert.True(scheduled[0]());
        Assert.Equal(3, scheduled.Count);
        Assert.True(scheduled[1]());
        Assert.True(scheduled[2]());

        Assert.Equal(["value:1", "value:2", "edge"], executed);
    }

    [Fact]
    public void Reset_InvalidatesStaleScheduledWork()
    {
        var queue = new LatestQueuedCommand<int>();
        var scheduled = new List<Func<bool>>();
        var executed = new List<int>();
        bool Schedule(Func<bool> action)
        {
            scheduled.Add(action);
            return true;
        }
        bool Execute(int value)
        {
            executed.Add(value);
            return true;
        }

        Assert.True(queue.Enqueue(1, Schedule, Execute));
        queue.Reset();
        Assert.True(queue.Enqueue(2, Schedule, Execute));

        Assert.False(scheduled[0]());
        Assert.True(scheduled[1]());
        Assert.Equal([2], executed);
    }

    [Fact]
    public void Enqueue_ReportsSchedulerBackpressure()
    {
        var queue = new LatestQueuedCommand<int>();

        Assert.False(queue.Enqueue(
            1,
            _ => false,
            _ => true));
        Assert.False(queue.Enqueue(
            2,
            _ => false,
            _ => true));
    }

    [Fact]
    public void KeyedQueue_CoalescesEachViewportIndependently()
    {
        var queue = new KeyedLatestQueuedCommand<ulong, int>();
        var scheduled = new List<Func<bool>>();
        var executed = new List<int>();

        bool Schedule(Func<bool> action)
        {
            scheduled.Add(action);
            return true;
        }
        bool Execute(int value)
        {
            executed.Add(value);
            return true;
        }

        Assert.True(queue.Enqueue(1, 10, Schedule, Execute));
        Assert.True(queue.Enqueue(2, 20, Schedule, Execute));
        Assert.True(queue.Enqueue(1, 11, Schedule, Execute));
        Assert.True(queue.Enqueue(2, 21, Schedule, Execute));

        Assert.Equal(2, scheduled.Count);
        Assert.All(scheduled, action => Assert.True(action()));
        Assert.Equal([11, 21], executed);
    }

    [Fact]
    public void KeyedQueue_ResetInvalidatesEveryPendingViewport()
    {
        var queue = new KeyedLatestQueuedCommand<ulong, int>();
        var scheduled = new List<Func<bool>>();
        var executed = new List<int>();
        bool Schedule(Func<bool> action)
        {
            scheduled.Add(action);
            return true;
        }

        Assert.True(queue.Enqueue(
            1,
            10,
            Schedule,
            value =>
            {
                executed.Add(value);
                return true;
            }));
        Assert.True(queue.Enqueue(
            2,
            20,
            Schedule,
            value =>
            {
                executed.Add(value);
                return true;
            }));

        queue.Reset();

        Assert.All(scheduled, action => Assert.False(action()));
        Assert.Empty(executed);
    }
}
