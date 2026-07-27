using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;

namespace SailorEditor.Protocol;

internal interface ILocalEngineProtocolNativeBridge
{
    int StartLocalHost(
        byte[] requestData,
        ushort port,
        string authorizationToken);
    void RequestLocalHostStop();
    void StopLocalHost(bool shutdownEngine);
}

internal sealed class LocalEngineProtocolNativeBridge :
    ILocalEngineProtocolNativeBridge
{
    public static LocalEngineProtocolNativeBridge Instance { get; } = new();

    LocalEngineProtocolNativeBridge()
    {
    }

    public int StartLocalHost(
        byte[] requestData,
        ushort port,
        string authorizationToken)
        => EngineProtocolNative.SailorProtocolStartLocalHost(
            requestData,
            checked((uint)requestData.Length),
            port,
            authorizationToken,
            checked((uint)authorizationToken.Length));

    public void RequestLocalHostStop()
        => EngineProtocolNative.SailorProtocolRequestLocalHostStop();

    public void StopLocalHost(bool shutdownEngine)
        => EngineProtocolNative.SailorProtocolStopLocalHost(shutdownEngine);
}

internal sealed class LocalEngineProtocolTransport :
    IEngineProtocolTransport,
    ILocalEngineProtocolTransport,
    IAsyncDisposable
{
    sealed record LocalHostEndpoint(Uri Endpoint, string AuthorizationToken)
    {
        public int InFlightStopRequests { get; set; }
        public bool IsStopping { get; set; }
    }

    const int MaxBindAttempts = 8;
    static readonly object hostGate = new();
    static LocalHostEndpoint? activeHost;

    readonly object stateGate = new();
    readonly TaskCompletionSource<bool> disposalCompletion =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    readonly ILocalEngineProtocolNativeBridge nativeBridge;
    readonly Func<Uri, string, IEngineProtocolTransport> transportFactory;
    IEngineProtocolTransport? webSocketTransport;
    LocalHostEndpoint? initializingHost;
    LocalHostEndpoint? ownedHost;
    bool initializing;
    int pendingHostShutdowns;
    int disposed;

    public LocalEngineProtocolTransport()
        : this(
            LocalEngineProtocolNativeBridge.Instance,
            static (endpoint, authorizationToken) =>
                new ClientWebSocketEngineProtocolTransport(
                    endpoint,
                    authorizationToken))
    {
    }

    internal LocalEngineProtocolTransport(
        ILocalEngineProtocolNativeBridge nativeBridge,
        Func<Uri, string, IEngineProtocolTransport> transportFactory)
    {
        this.nativeBridge = nativeBridge ??
            throw new ArgumentNullException(nameof(nativeBridge));
        this.transportFactory = transportFactory ??
            throw new ArgumentNullException(nameof(transportFactory));
    }

    public void Initialize(byte[] requestData)
    {
        ArgumentNullException.ThrowIfNull(requestData);
        lock (stateGate)
        {
            ThrowIfDisposed();
            if (initializing ||
                ownedHost is not null ||
                webSocketTransport is not null)
            {
                throw new EngineProtocolException(
                    "The local Engine WebSocket transport is already initialized.");
            }
            initializing = true;
        }

        LocalHostEndpoint? candidateHost = null;
        IEngineProtocolTransport? candidateTransport = null;
        try
        {
            lock (hostGate)
            {
                ThrowIfDisposed();
                if (activeHost is not null)
                {
                    throw new EngineProtocolException(
                        "The local Engine WebSocket host is already initialized.");
                }
                candidateHost = StartNewHost(requestData);
            }

            lock (stateGate)
            {
                initializingHost = candidateHost;
                ThrowIfDisposed();
            }

            candidateTransport = transportFactory(
                candidateHost.Endpoint,
                candidateHost.AuthorizationToken) ??
                throw new EngineProtocolException(
                    "The local Engine WebSocket transport factory returned null.");

            lock (stateGate)
            {
                ThrowIfDisposed();
                if (!ReferenceEquals(initializingHost, candidateHost))
                {
                    throw new EngineProtocolException(
                        "Local Engine WebSocket host ownership changed during initialization.");
                }

                initializingHost = null;
                ownedHost = candidateHost;
                webSocketTransport = candidateTransport;
                candidateHost = null;
                candidateTransport = null;
            }
        }
        catch
        {
            LocalHostEndpoint? hostToStop = null;
            var shutdownAsPartOfDisposal = false;
            if (candidateHost is not null)
            {
                lock (stateGate)
                {
                    if (ReferenceEquals(initializingHost, candidateHost))
                    {
                        initializingHost = null;
                        hostToStop = candidateHost;
                        shutdownAsPartOfDisposal =
                            Volatile.Read(ref disposed) != 0;
                        if (shutdownAsPartOfDisposal)
                        {
                            pendingHostShutdowns += 1;
                        }
                    }
                }
            }

            if (shutdownAsPartOfDisposal && hostToStop is not null)
            {
                BeginNonBlockingHostShutdown(
                    hostToStop,
                    candidateTransport);
                candidateTransport = null;
            }
            else
            {
                try
                {
                    candidateTransport?.Dispose();
                }
                finally
                {
                    if (hostToStop is not null)
                    {
                        StopOwnedHost(
                            hostToStop,
                            shutdownEngine: true);
                    }
                }
            }
            throw;
        }
        finally
        {
            lock (stateGate)
            {
                initializing = false;
                TryCompleteDisposalUnderLock();
            }
        }
    }

    LocalHostEndpoint StartNewHost(byte[] requestData)
    {
        var token = Convert.ToHexString(
            RandomNumberGenerator.GetBytes(32));
        for (var attempt = 0; attempt < MaxBindAttempts; ++attempt)
        {
            ThrowIfDisposed();
            var port = ReserveLoopbackPort();
            var candidate = new LocalHostEndpoint(
                new Uri($"ws://127.0.0.1:{port}/sailor/editor/v1"),
                token);
            var status = nativeBridge.StartLocalHost(
                requestData,
                checked((ushort)port),
                token);
            if (status == 0)
            {
                activeHost = candidate;
                return candidate;
            }
            if (status != 3)
            {
                ThrowIfHostFailed(status);
            }
        }

        throw new EngineProtocolException(
            "Cannot bind the local Engine WebSocket host after multiple attempts.");
    }

    static int ReserveLoopbackPort()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        try
        {
            return ((IPEndPoint)listener.LocalEndpoint).Port;
        }
        finally
        {
            listener.Stop();
        }
    }

    static void ThrowIfHostFailed(int status)
    {
        if (status == 0)
        {
            return;
        }

        var message = status switch
        {
            1 => "Local Engine WebSocket host rejected its bootstrap arguments.",
            2 => "A different local Engine WebSocket host is already running.",
            3 => "Local Engine WebSocket host could not bind its loopback port.",
            4 => "Local Engine initialization failed.",
            5 => "Local Engine WebSocket host failed during bootstrap.",
            6 => "Local Engine WebSocket networking could not be initialized.",
            _ => $"Local Engine WebSocket host failed with status {status}."
        };
        throw new EngineProtocolException(message);
    }

    public byte[] Invoke(
        byte[] requestData,
        EngineProtocolInvocationKind invocationKind,
        CancellationToken cancellationToken = default)
    {
        IEngineProtocolTransport transport;
        lock (stateGate)
        {
            ThrowIfDisposed();
            transport = webSocketTransport ??
                throw new EngineProtocolException(
                    "Local Engine WebSocket host has not been initialized.");
        }
        return transport.Invoke(
            requestData,
            invocationKind,
            cancellationToken);
    }

    public void RequestStopFallback()
    {
        LocalHostEndpoint host;
        lock (stateGate)
        {
            ThrowIfDisposed();
            host = ownedHost ??
                throw new EngineProtocolException(
                    "Local Engine WebSocket host has not been initialized.");
        }
        RequestOwnedHostStop(host);
    }

    public void CompleteShutdown(bool shutdownEngine)
    {
        IEngineProtocolTransport? transport;
        LocalHostEndpoint? hostToStop = null;
        lock (stateGate)
        {
            ThrowIfDisposed();
            if (initializing)
            {
                throw new EngineProtocolException(
                    "Local Engine WebSocket transport is still initializing.");
            }

            transport = webSocketTransport;
            webSocketTransport = null;
            hostToStop = ownedHost;
            ownedHost = null;
            if (hostToStop is not null)
            {
                pendingHostShutdowns += 1;
            }
        }

        try
        {
            transport?.Dispose();
        }
        finally
        {
            if (hostToStop is not null)
            {
                try
                {
                    StopOwnedHost(hostToStop, shutdownEngine);
                }
                finally
                {
                    CompleteHostShutdown();
                }
            }
        }
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref disposed, 1) != 0)
        {
            return;
        }

        IEngineProtocolTransport? transport;
        LocalHostEndpoint? hostToStop = null;
        lock (stateGate)
        {
            hostToStop = ownedHost ?? initializingHost;
            transport = webSocketTransport;
            webSocketTransport = null;
            initializingHost = null;
            ownedHost = null;
            if (hostToStop is not null)
            {
                pendingHostShutdowns += 1;
            }
        }

        if (hostToStop is not null)
        {
            BeginNonBlockingHostShutdown(hostToStop, transport);
            return;
        }

        try
        {
            transport?.Dispose();
        }
        finally
        {
            CompleteDisposalIfReady();
        }
    }

    public ValueTask DisposeAsync()
    {
        Dispose();
        return new ValueTask(disposalCompletion.Task);
    }

    void BeginNonBlockingHostShutdown(
        LocalHostEndpoint host,
        IEngineProtocolTransport? transport)
    {
        try
        {
            RequestOwnedHostStop(host);
        }
        catch (Exception exception)
        {
            Console.WriteLine(
                $"[EngineProtocol] Local host stop request failed: {exception.Message}");
        }

        try
        {
            transport?.Dispose();
        }
        catch (Exception exception)
        {
            Console.WriteLine(
                $"[EngineProtocol] Local WebSocket disposal failed: {exception.Message}");
        }

        QueueOwnedHostShutdown(host);
    }

    void QueueOwnedHostShutdown(LocalHostEndpoint host)
    {
        _ = Task.Run(() =>
        {
            try
            {
                StopOwnedHost(host, shutdownEngine: true);
            }
            catch (Exception exception)
            {
                Console.WriteLine(
                    $"[EngineProtocol] Local host teardown failed: {exception.Message}");
            }
            finally
            {
                CompleteHostShutdown();
            }
        });
    }

    void CompleteHostShutdown()
    {
        lock (stateGate)
        {
            pendingHostShutdowns -= 1;
            TryCompleteDisposalUnderLock();
        }
    }

    void CompleteDisposalIfReady()
    {
        lock (stateGate)
        {
            TryCompleteDisposalUnderLock();
        }
    }

    void TryCompleteDisposalUnderLock()
    {
        if (Volatile.Read(ref disposed) != 0 &&
            !initializing &&
            pendingHostShutdowns == 0)
        {
            disposalCompletion.TrySetResult(true);
        }
    }

    void RequestOwnedHostStop(LocalHostEndpoint host)
    {
        lock (hostGate)
        {
            if (!ReferenceEquals(activeHost, host) ||
                host.IsStopping)
            {
                return;
            }
            host.InFlightStopRequests += 1;
        }

        try
        {
            nativeBridge.RequestLocalHostStop();
        }
        finally
        {
            lock (hostGate)
            {
                host.InFlightStopRequests -= 1;
                if (host.InFlightStopRequests == 0)
                {
                    Monitor.PulseAll(hostGate);
                }
            }
        }
    }

    void StopOwnedHost(
        LocalHostEndpoint host,
        bool shutdownEngine)
    {
        var claimedHost = false;
        try
        {
            lock (hostGate)
            {
                if (!ReferenceEquals(activeHost, host) ||
                    host.IsStopping)
                {
                    return;
                }
                host.IsStopping = true;
                claimedHost = true;
                while (host.InFlightStopRequests != 0)
                {
                    Monitor.Wait(hostGate);
                }
            }

            nativeBridge.StopLocalHost(shutdownEngine);
        }
        finally
        {
            if (claimedHost)
            {
                lock (hostGate)
                {
                    if (ReferenceEquals(activeHost, host))
                    {
                        activeHost = null;
                    }
                }
            }
        }
    }

    void ThrowIfDisposed()
        => ObjectDisposedException.ThrowIf(
            Volatile.Read(ref disposed) != 0,
            this);
}
