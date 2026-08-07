#nullable enable

namespace SailorEditor.Mcp;

public interface IEditorThreadDispatcher
{
    Task InvokeAsync(
        Func<Task> action,
        CancellationToken cancellationToken = default);

    Task<T> InvokeAsync<T>(
        Func<Task<T>> action,
        CancellationToken cancellationToken = default);
}

public sealed class MauiEditorThreadDispatcher : IEditorThreadDispatcher
{
    public async Task InvokeAsync(
        Func<Task> action,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(action);
        cancellationToken.ThrowIfCancellationRequested();
        if (MainThread.IsMainThread)
        {
            await action();
            return;
        }

        await MainThread.InvokeOnMainThreadAsync(async () =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            await action();
        });
    }

    public async Task<T> InvokeAsync<T>(
        Func<Task<T>> action,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(action);
        cancellationToken.ThrowIfCancellationRequested();
        if (MainThread.IsMainThread)
            return await action();

        return await MainThread.InvokeOnMainThreadAsync(async () =>
        {
            cancellationToken.ThrowIfCancellationRequested();
            return await action();
        });
    }
}
