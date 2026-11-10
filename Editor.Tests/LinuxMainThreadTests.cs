using SailorEditor;

namespace Editor.Tests;

public sealed class LinuxMainThreadTests
{
    sealed class QueuedContext : SynchronizationContext
    {
        readonly Queue<(SendOrPostCallback Callback, object? State)> pending = new();

        public override void Post(SendOrPostCallback callback, object? state)
        {
            lock (pending)
            {
                pending.Enqueue((callback, state));
            }
        }

        public void Drain()
        {
            while (true)
            {
                (SendOrPostCallback Callback, object? State) item;
                lock (pending)
                {
                    if (pending.Count == 0)
                    {
                        return;
                    }
                    item = pending.Dequeue();
                }
                item.Callback(item.State);
            }
        }
    }

    [Fact]
    public void BackgroundWorkRunsOnCapturedUiThreadAndPropagatesFailure()
    {
        var previous = SynchronizationContext.Current;
        var context = new QueuedContext();
        SynchronizationContext.SetSynchronizationContext(context);
        try
        {
            LinuxMainThread.Initialize();
            var uiThread = Environment.CurrentManagedThreadId;
            Task<int>? work = null;
            var worker = new Thread(() =>
            {
                Assert.False(LinuxMainThread.IsMainThread);
                work = LinuxMainThread.InvokeOnMainThreadAsync(() => Environment.CurrentManagedThreadId);
            });
            worker.Start();
            worker.Join();
            Assert.False(work!.IsCompleted);
            context.Drain();
            Assert.Equal(uiThread, work.GetAwaiter().GetResult());

            Task<int>? failure = null;
            var failingWorker = new Thread(() =>
                failure = LinuxMainThread.InvokeOnMainThreadAsync<int>((Func<int>)(() => throw new InvalidOperationException("UI operation failed"))));
            failingWorker.Start();
            failingWorker.Join();
            context.Drain();
            var exception = Assert.Throws<InvalidOperationException>(() => failure!.GetAwaiter().GetResult());
            Assert.Equal("UI operation failed", exception.Message);
        }
        finally
        {
            SynchronizationContext.SetSynchronizationContext(previous);
        }
    }
}
