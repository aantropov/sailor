namespace SailorEditor;

// MAUI's portable MainThread facade has no Linux implementation. GTK supplies
// this synchronization context before it creates the MAUI application.
internal static class LinuxMainThread
{
    static SynchronizationContext? context;
    static int threadId;

    internal static void Initialize()
    {
        context = SynchronizationContext.Current
            ?? throw new InvalidOperationException("GTK did not install its UI synchronization context.");
        threadId = Environment.CurrentManagedThreadId;
    }

    public static bool IsMainThread => context is not null && Environment.CurrentManagedThreadId == threadId;

    public static void BeginInvokeOnMainThread(Action action)
    {
        ArgumentNullException.ThrowIfNull(action);
        if (IsMainThread)
        {
            action();
            return;
        }

        var dispatcher = context ?? throw new InvalidOperationException("The GTK UI thread has not started.");
        dispatcher.Post(_ => action(), null);
    }

    public static Task InvokeOnMainThreadAsync(Action action) =>
        InvokeOnMainThreadAsync(() =>
        {
            action();
            return Task.CompletedTask;
        });

    public static Task<T> InvokeOnMainThreadAsync<T>(Func<T> action) =>
        InvokeOnMainThreadAsync(() => Task.FromResult(action()));

    public static Task InvokeOnMainThreadAsync(Func<Task> action) =>
        InvokeOnMainThreadAsync(async () =>
        {
            await action();
            return true;
        });

    public static Task<T> InvokeOnMainThreadAsync<T>(Func<Task<T>> action)
    {
        ArgumentNullException.ThrowIfNull(action);
        if (IsMainThread)
        {
            return action();
        }

        var completion = new TaskCompletionSource<T>(TaskCreationOptions.RunContinuationsAsynchronously);
        BeginInvokeOnMainThread(async () =>
        {
            try
            {
                completion.TrySetResult(await action());
            }
            catch (OperationCanceledException exception)
            {
                completion.TrySetCanceled(exception.CancellationToken);
            }
            catch (Exception exception)
            {
                completion.TrySetException(exception);
            }
        });
        return completion.Task;
    }
}
