namespace SailorEditor.Protocol;

internal enum EngineProtocolInvocationKind
{
    Request,
    Interactive,
    Lifecycle,
    Background
}

internal interface IEngineProtocolTransport : IDisposable
{
    byte[] Invoke(
        byte[] requestData,
        EngineProtocolInvocationKind invocationKind,
        CancellationToken cancellationToken = default);
}

internal interface ILocalEngineProtocolTransport
{
    void Initialize(byte[] requestData);
    void RequestStopFallback();
    void CompleteShutdown(bool shutdownEngine);
}

internal delegate byte[] EngineProtocolInvokeDelegate(
    byte[] requestData,
    EngineProtocolInvocationKind invocationKind);

internal sealed class DelegateEngineProtocolTransport(
    EngineProtocolInvokeDelegate invoke) : IEngineProtocolTransport
{
    readonly EngineProtocolInvokeDelegate invoke =
        invoke ?? throw new ArgumentNullException(nameof(invoke));

    public byte[] Invoke(
        byte[] requestData,
        EngineProtocolInvocationKind invocationKind,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return invoke(requestData, invocationKind);
    }

    public void Dispose()
    {
    }
}
