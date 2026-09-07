using SailorEditor.Services;

namespace Editor.Tests;

public sealed class LatestQueuedCommandTests
{
    [Fact]
    public async Task Deduplication_SkipsOnlySuccessfulStateUntilReset()
    {
        var queue = new LatestQueuedCommand<int>(deduplicateSuccessfulValues: true);
        Func<ValueTask<bool>>? pending = null;
        var executed = new List<int>();
        var succeed = true;
        async Task<bool> Send(int value)
        {
            Assert.True(queue.Enqueue(value, action => { pending = action; return true; }, update =>
            {
                executed.Add(update);
                return ValueTask.FromResult(succeed);
            }));
            return await pending!();
        }

        Assert.True(await Send(1));
        Assert.True(await Send(1));
        Assert.Equal([1], executed);
        succeed = false;
        Assert.False(await Send(2));
        Assert.False(await Send(2));
        succeed = true;
        Assert.True(await Send(2));
        Assert.True(await Send(2));
        Assert.True(await Send(1));
        queue.Reset();
        Assert.True(await Send(1));
        Assert.Equal([1, 2, 2, 2, 1, 1], executed);
    }

    [Fact]
    public async Task Deduplication_RestoresEarlierStateAfterInFlightChange()
    {
        var queue = new LatestQueuedCommand<int>(deduplicateSuccessfulValues: true);
        var scheduled = new Queue<Func<ValueTask<bool>>>();
        var executed = new List<int>();
        var started = new TaskCompletionSource();
        var finish = new TaskCompletionSource<bool>();
        bool Send(int value) => queue.Enqueue(value, action => { scheduled.Enqueue(action); return true; }, async update =>
        {
            executed.Add(update);
            if (update == 2)
            {
                started.SetResult();
                return await finish.Task;
            }
            return true;
        });

        Assert.True(Send(1));
        Assert.True(await scheduled.Dequeue()());
        Assert.True(Send(2));
        var inFlight = scheduled.Dequeue()().AsTask();
        await started.Task;
        Assert.True(Send(1));
        finish.SetResult(true);
        Assert.True(await inFlight);
        Assert.True(await scheduled.Dequeue()());
        Assert.Equal([1, 2, 1], executed);
    }

    [Fact]
    public async Task Enqueue_CoalescesPendingValuesToTheLatest()
    {
        var queue = new LatestQueuedCommand<int>();
        var scheduled = new List<Func<ValueTask<bool>>>();
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
                return ValueTask.FromResult(true);
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
                return ValueTask.FromResult(true);
            }));

        Assert.Single(scheduled);
        Assert.True(await scheduled[0]());
        Assert.Equal([2], executed);
    }

    [Fact]
    public async Task Enqueue_PreservesValueArrivingDuringExecution()
    {
        var queue = new LatestQueuedCommand<int>();
        var scheduled = new List<Func<ValueTask<bool>>>();
        var executed = new List<int>();
        Func<Func<ValueTask<bool>>, bool> schedule = action =>
        {
            scheduled.Add(action);
            return true;
        };
        ValueTask<bool> Execute(int value)
        {
            executed.Add(value);
            if (value == 1)
            {
                Assert.True(queue.Enqueue(2, schedule, Execute));
            }
            return ValueTask.FromResult(true);
        }

        Assert.True(queue.Enqueue(1, schedule, Execute));
        Assert.True(await scheduled[0]());
        Assert.Equal(2, scheduled.Count);
        Assert.True(await scheduled[1]());
        Assert.Equal([1, 2], executed);
    }

    [Fact]
    public async Task Enqueue_ValueArrivingDuringExecutionPrecedesLaterEdgeCommand()
    {
        var queue = new LatestQueuedCommand<int>();
        var scheduled = new List<Func<ValueTask<bool>>>();
        var executed = new List<string>();
        bool Schedule(Func<ValueTask<bool>> action)
        {
            scheduled.Add(action);
            return true;
        }
        ValueTask<bool> Execute(int value)
        {
            executed.Add($"value:{value}");
            if (value == 1)
            {
                Assert.True(queue.Enqueue(2, Schedule, Execute));
                Assert.True(Schedule(() =>
                {
                    executed.Add("edge");
                    return ValueTask.FromResult(true);
                }));
            }
            return ValueTask.FromResult(true);
        }

        Assert.True(queue.Enqueue(1, Schedule, Execute));
        Assert.True(await scheduled[0]());
        Assert.Equal(3, scheduled.Count);
        Assert.True(await scheduled[1]());
        Assert.True(await scheduled[2]());

        Assert.Equal(["value:1", "value:2", "edge"], executed);
    }

    [Fact]
    public async Task Reset_InvalidatesStaleScheduledWork()
    {
        var queue = new LatestQueuedCommand<int>();
        var scheduled = new List<Func<ValueTask<bool>>>();
        var executed = new List<int>();
        bool Schedule(Func<ValueTask<bool>> action)
        {
            scheduled.Add(action);
            return true;
        }
        ValueTask<bool> Execute(int value)
        {
            executed.Add(value);
            return ValueTask.FromResult(true);
        }

        Assert.True(queue.Enqueue(1, Schedule, Execute));
        queue.Reset();
        Assert.True(queue.Enqueue(2, Schedule, Execute));

        Assert.False(await scheduled[0]());
        Assert.True(await scheduled[1]());
        Assert.Equal([2], executed);
    }

    [Fact]
    public void Enqueue_ReportsSchedulerBackpressure()
    {
        var queue = new LatestQueuedCommand<int>();

        Assert.False(queue.Enqueue(
            1,
            _ => false,
            _ => ValueTask.FromResult(true)));
        Assert.False(queue.Enqueue(
            2,
            _ => false,
            _ => ValueTask.FromResult(true)));
    }

    [Fact]
    public async Task KeyedQueue_CoalescesEachViewportIndependently()
    {
        var queue = new KeyedLatestQueuedCommand<ulong, int>();
        var scheduled = new List<Func<ValueTask<bool>>>();
        var executed = new List<int>();

        bool Schedule(Func<ValueTask<bool>> action)
        {
            scheduled.Add(action);
            return true;
        }
        ValueTask<bool> Execute(int value)
        {
            executed.Add(value);
            return ValueTask.FromResult(true);
        }

        Assert.True(queue.Enqueue(1, 10, Schedule, Execute));
        Assert.True(queue.Enqueue(2, 20, Schedule, Execute));
        Assert.True(queue.Enqueue(1, 11, Schedule, Execute));
        Assert.True(queue.Enqueue(2, 21, Schedule, Execute));

        Assert.Equal(2, scheduled.Count);
        foreach (var action in scheduled)
        {
            Assert.True(await action());
        }
        Assert.Equal([11, 21], executed);
    }

    [Fact]
    public async Task KeyedQueue_ResetInvalidatesEveryPendingViewport()
    {
        var queue = new KeyedLatestQueuedCommand<ulong, int>();
        var scheduled = new List<Func<ValueTask<bool>>>();
        var executed = new List<int>();
        bool Schedule(Func<ValueTask<bool>> action)
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
                return ValueTask.FromResult(true);
            }));
        Assert.True(queue.Enqueue(
            2,
            20,
            Schedule,
            value =>
            {
                executed.Add(value);
                return ValueTask.FromResult(true);
            }));

        queue.Reset();

        foreach (var action in scheduled)
        {
            Assert.False(await action());
        }
        Assert.Empty(executed);
    }
}
