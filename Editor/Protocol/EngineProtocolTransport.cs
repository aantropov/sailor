namespace SailorEditor.Protocol;

internal enum EngineProtocolInvocationKind
{
    Request,
    Interactive,
    Lifecycle,
    Background
}

internal interface IEngineProtocolTransport : IDisposable, IAsyncDisposable
{
    Task<byte[]> InvokeAsync(
        byte[] requestData,
        EngineProtocolInvocationKind invocationKind,
        CancellationToken cancellationToken = default);
}

internal interface ILocalEngineProtocolTransport
{
    Task InitializeAsync(
        byte[] requestData,
        CancellationToken cancellationToken = default);
    Task RequestStopFallbackAsync();
    Task CompleteShutdownAsync(bool shutdownEngine);
}

internal delegate Task<byte[]> EngineProtocolInvokeAsyncDelegate(
    byte[] requestData,
    EngineProtocolInvocationKind invocationKind,
    CancellationToken cancellationToken);

internal sealed class DelegateEngineProtocolTransport(
    EngineProtocolInvokeAsyncDelegate invoke) : IEngineProtocolTransport
{
    readonly EngineProtocolInvokeAsyncDelegate invoke =
        invoke ?? throw new ArgumentNullException(nameof(invoke));

    public Task<byte[]> InvokeAsync(
        byte[] requestData,
        EngineProtocolInvocationKind invocationKind,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return invoke(
            requestData,
            invocationKind,
            cancellationToken);
    }

    public void Dispose()
    {
    }

    public ValueTask DisposeAsync()
        => ValueTask.CompletedTask;
}
