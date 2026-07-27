using System.Net.WebSockets;

namespace SailorEditor.Protocol;

internal sealed class ClientWebSocketEngineProtocolTransport : IEngineProtocolTransport
{
    sealed class WebSocketLane(
        string name,
        bool allowsBlockingRequest = false)
    {
        public string Name { get; } = name;
        public bool AllowsBlockingRequest { get; } = allowsBlockingRequest;
        public SemaphoreSlim Gate { get; } = new(1, 1);
        public ClientWebSocket? Socket;
    }

    const string ProtocolSubprotocol = "sailor.editor.v1";
    static readonly TimeSpan DefaultConnectTimeout = TimeSpan.FromSeconds(10);
    static readonly TimeSpan DefaultRequestTimeout = TimeSpan.FromMinutes(2);
    static readonly TimeSpan InteractiveRequestTimeout =
        TimeSpan.FromSeconds(5);
    static readonly TimeSpan LifecycleRequestTimeout =
        TimeSpan.FromSeconds(15);
    static readonly TimeSpan KeepAliveInterval =
        TimeSpan.FromSeconds(10);
    static readonly TimeSpan KeepAliveTimeout =
        TimeSpan.FromSeconds(10);
    static readonly TimeSpan LaneDisposeGracePeriod =
        TimeSpan.FromMilliseconds(100);
    static readonly TimeSpan LaneAbortDrainTimeout =
        TimeSpan.FromMilliseconds(400);

    readonly Uri endpoint;
    readonly string authorizationHeader;
    readonly TimeSpan connectTimeout;
    readonly TimeSpan requestTimeout;
    readonly WebSocketLane requestLane = new("request");
    readonly WebSocketLane interactiveLane = new("interactive");
    readonly WebSocketLane lifecycleLane = new("lifecycle");
    readonly WebSocketLane backgroundLane =
        new("background", allowsBlockingRequest: true);
    readonly CancellationTokenSource disposeCancellation = new();
    int disposed;
    int activeInvocations;

    public ClientWebSocketEngineProtocolTransport(
        Uri endpoint,
        string authorizationToken,
        TimeSpan? connectTimeout = null,
        TimeSpan? requestTimeout = null)
    {
        this.endpoint = ValidateEndpoint(endpoint);
        authorizationHeader = $"Bearer {ValidateToken(authorizationToken)}";
        this.connectTimeout = ValidateTimeout(
            connectTimeout ?? DefaultConnectTimeout,
            nameof(connectTimeout));
        this.requestTimeout = ValidateTimeout(
            requestTimeout ?? DefaultRequestTimeout,
            nameof(requestTimeout));
    }

    public byte[] Invoke(
        byte[] requestData,
        EngineProtocolInvocationKind invocationKind,
        CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(
            Volatile.Read(ref disposed) != 0,
            this);
        ArgumentNullException.ThrowIfNull(requestData);
        if (requestData.Length == 0 ||
            requestData.Length > EngineProtocolClient.MaxPayloadSize)
        {
            throw new EngineProtocolException(
                "Engine WebSocket request is outside the protocol payload limit.");
        }

        EnterInvocation();
        try
        {
            return InvokeTracked(
                requestData,
                invocationKind,
                cancellationToken);
        }
        finally
        {
            Interlocked.Decrement(ref activeInvocations);
        }
    }

    byte[] InvokeTracked(
        byte[] requestData,
        EngineProtocolInvocationKind invocationKind,
        CancellationToken cancellationToken)
    {
        var lane = invocationKind switch
        {
            EngineProtocolInvocationKind.Interactive => interactiveLane,
            EngineProtocolInvocationKind.Lifecycle => lifecycleLane,
            EngineProtocolInvocationKind.Background => backgroundLane,
            _ => requestLane
        };
        var timeout = invocationKind switch
        {
            EngineProtocolInvocationKind.Background =>
                Timeout.InfiniteTimeSpan,
            EngineProtocolInvocationKind.Interactive =>
                InteractiveRequestTimeout,
            EngineProtocolInvocationKind.Lifecycle =>
                LifecycleRequestTimeout,
            _ => requestTimeout
        };

        using var invocationCancellation =
            CreateOperationCancellation(timeout, cancellationToken);
        var acquired = false;
        try
        {
            lane.Gate.Wait(invocationCancellation.Token);
            acquired = true;
            ObjectDisposedException.ThrowIf(
                Volatile.Read(ref disposed) != 0,
                this);
            return InvokeUnderLock(
                lane,
                requestData,
                invocationCancellation.Token,
                cancellationToken);
        }
        catch (OperationCanceledException ex)
        {
            if (cancellationToken.IsCancellationRequested)
            {
                throw new OperationCanceledException(
                    $"Engine WebSocket {lane.Name} request was cancelled.",
                    ex,
                    cancellationToken);
            }
            throw CreateCancellationException(
                lane,
                ex);
        }
        finally
        {
            if (acquired)
            {
                lane.Gate.Release();
            }
        }
    }

    void EnterInvocation()
    {
        Interlocked.Increment(ref activeInvocations);
        if (Volatile.Read(ref disposed) == 0)
        {
            return;
        }

        Interlocked.Decrement(ref activeInvocations);
        throw new ObjectDisposedException(
            nameof(ClientWebSocketEngineProtocolTransport));
    }

    byte[] InvokeUnderLock(
        WebSocketLane lane,
        byte[] requestData,
        CancellationToken invocationCancellation,
        CancellationToken callerCancellation)
    {
        try
        {
            var socket = EnsureConnected(
                lane,
                invocationCancellation);
            socket.SendAsync(
                    requestData.AsMemory(),
                    WebSocketMessageType.Binary,
                    endOfMessage: true,
                    invocationCancellation)
                .AsTask()
                .GetAwaiter()
                .GetResult();

            using var response = new MemoryStream();
            var receiveBuffer = new byte[64 * 1024];
            while (true)
            {
                var result = socket.ReceiveAsync(
                        receiveBuffer.AsMemory(),
                        invocationCancellation)
                    .AsTask()
                    .GetAwaiter()
                    .GetResult();
                if (result.MessageType == WebSocketMessageType.Close)
                {
                    throw new EngineProtocolException(
                        $"Engine WebSocket closed the {lane.Name} channel " +
                        $"({socket.CloseStatus}: {socket.CloseStatusDescription}).");
                }
                if (result.MessageType != WebSocketMessageType.Binary)
                {
                    throw new EngineProtocolException(
                        "Engine WebSocket returned a non-binary protocol message.");
                }
                if (response.Length + result.Count >
                    EngineProtocolClient.MaxPayloadSize)
                {
                    throw new EngineProtocolException(
                        "Engine WebSocket response exceeds the protocol payload limit.");
                }

                response.Write(receiveBuffer, 0, result.Count);
                if (result.EndOfMessage)
                {
                    break;
                }
            }

            if (response.Length == 0)
            {
                throw new EngineProtocolException(
                    "Engine WebSocket returned an empty protocol response.");
            }
            return response.ToArray();
        }
        catch (OperationCanceledException ex)
        {
            ResetLane(lane);
            if (callerCancellation.IsCancellationRequested)
            {
                throw new OperationCanceledException(
                    $"Engine WebSocket {lane.Name} request was cancelled.",
                    ex,
                    callerCancellation);
            }
            throw CreateCancellationException(
                lane,
                ex);
        }
        catch (EngineProtocolException)
        {
            ResetLane(lane);
            throw;
        }
        catch (Exception ex) when (
            ex is WebSocketException or
                IOException or
                InvalidOperationException or
                ObjectDisposedException)
        {
            ResetLane(lane);
            throw new EngineProtocolException(
                $"Engine WebSocket {lane.Name} channel failed.",
                ex);
        }
    }

    ClientWebSocket EnsureConnected(
        WebSocketLane lane,
        CancellationToken invocationCancellation)
    {
        var existingSocket = Volatile.Read(ref lane.Socket);
        if (existingSocket?.State == WebSocketState.Open)
        {
            return existingSocket;
        }

        ResetLane(lane);
        var socket = new ClientWebSocket();

        try
        {
            socket.Options.AddSubProtocol(ProtocolSubprotocol);
            socket.Options.SetRequestHeader(
                "Authorization",
                authorizationHeader);
            socket.Options.SetRequestHeader(
                "X-Sailor-Channel",
                lane.Name);
            // A finite timeout makes a half-open remote connection observable.
            // A blocking background command is executed synchronously by the
            // current native server callback, which cannot service PING until
            // the command completes. Caller cancellation and lane abort still
            // bound that channel without a false keep-alive failure.
            socket.Options.KeepAliveInterval = KeepAliveInterval;
            socket.Options.KeepAliveTimeout =
                lane.AllowsBlockingRequest
                    ? Timeout.InfiniteTimeSpan
                    : KeepAliveTimeout;
            using var connectCancellation =
                CancellationTokenSource.CreateLinkedTokenSource(
                    invocationCancellation);
            connectCancellation.CancelAfter(connectTimeout);
            socket.ConnectAsync(endpoint, connectCancellation.Token)
                .GetAwaiter()
                .GetResult();
            Interlocked.Exchange(ref lane.Socket, socket);
            if (Volatile.Read(ref disposed) != 0)
            {
                ResetLane(lane);
                throw new ObjectDisposedException(
                    nameof(ClientWebSocketEngineProtocolTransport));
            }
            return socket;
        }
        catch (OperationCanceledException)
        {
            socket.Dispose();
            throw;
        }
        catch (Exception ex) when (
            ex is WebSocketException or
                ArgumentException or
                FormatException or
                InvalidOperationException)
        {
            socket.Dispose();
            throw new EngineProtocolException(
                $"Cannot connect to Engine WebSocket endpoint '{endpoint}'.",
                ex);
        }
    }

    CancellationTokenSource CreateOperationCancellation(
        TimeSpan timeout,
        CancellationToken callerCancellation)
    {
        var cancellation = CancellationTokenSource.CreateLinkedTokenSource(
            disposeCancellation.Token,
            callerCancellation);
        if (timeout != Timeout.InfiniteTimeSpan)
        {
            cancellation.CancelAfter(timeout);
        }
        return cancellation;
    }

    EngineProtocolException CreateCancellationException(
        WebSocketLane lane,
        OperationCanceledException exception)
        => new(
            Volatile.Read(ref disposed) != 0
                ? $"Engine WebSocket {lane.Name} channel was closed."
                : $"Engine WebSocket {lane.Name} request timed out.",
            exception);

    static Uri ValidateEndpoint(Uri? endpoint)
    {
        ArgumentNullException.ThrowIfNull(endpoint);
        if (!endpoint.IsAbsoluteUri ||
            (endpoint.Scheme != Uri.UriSchemeWs &&
             endpoint.Scheme != Uri.UriSchemeWss))
        {
            throw new ArgumentException(
                "Engine protocol endpoint must use ws:// or wss://.",
                nameof(endpoint));
        }
        if (endpoint.Scheme == Uri.UriSchemeWs && !endpoint.IsLoopback)
        {
            throw new ArgumentException(
                "Plaintext Engine protocol endpoints are allowed only on loopback.",
                nameof(endpoint));
        }
        if (endpoint.AbsolutePath != "/sailor/editor/v1")
        {
            throw new ArgumentException(
                "Engine protocol endpoint path must be '/sailor/editor/v1'.",
                nameof(endpoint));
        }
        if (!string.IsNullOrEmpty(endpoint.UserInfo) ||
            !string.IsNullOrEmpty(endpoint.Query) ||
            !string.IsNullOrEmpty(endpoint.Fragment))
        {
            throw new ArgumentException(
                "Engine protocol endpoint must not contain credentials, a query, or a fragment.",
                nameof(endpoint));
        }
        return endpoint;
    }

    static string ValidateToken(string? token)
    {
        if (string.IsNullOrWhiteSpace(token) ||
            token.Length < 32 ||
            token.Length > 256 ||
            token.Any(character =>
                !char.IsAsciiLetterOrDigit(character) &&
                character is not '-' and not '_'))
        {
            throw new ArgumentException(
                "Engine protocol authorization token is invalid.",
                nameof(token));
        }
        return token;
    }

    static TimeSpan ValidateTimeout(TimeSpan timeout, string parameterName)
        => timeout > TimeSpan.Zero && timeout != Timeout.InfiniteTimeSpan
            ? timeout
            : throw new ArgumentOutOfRangeException(parameterName);

    static void ResetLane(WebSocketLane lane)
    {
        var socket = Interlocked.Exchange(
            ref lane.Socket,
            null);
        if (socket is null)
        {
            return;
        }

        try
        {
            socket.Abort();
        }
        catch
        {
        }
        socket.Dispose();
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref disposed, 1) != 0)
        {
            return;
        }
        disposeCancellation.Cancel();

        var allLanesQuiesced =
            DisposeLane(requestLane) &
            DisposeLane(interactiveLane) &
            DisposeLane(lifecycleLane) &
            DisposeLane(backgroundLane);
        if (allLanesQuiesced &&
            Volatile.Read(ref activeInvocations) == 0)
        {
            requestLane.Gate.Dispose();
            interactiveLane.Gate.Dispose();
            lifecycleLane.Gate.Dispose();
            backgroundLane.Gate.Dispose();
            disposeCancellation.Dispose();
        }
    }

    static bool DisposeLane(WebSocketLane lane)
    {
        if (!lane.Gate.Wait(LaneDisposeGracePeriod))
        {
            // Cancellation should normally release the owner. Abort the socket
            // without the lane lock as a fail-safe for a half-open native I/O
            // operation, then give the owner one final bounded drain window.
            ResetLane(lane);
            if (!lane.Gate.Wait(LaneAbortDrainTimeout))
            {
                return false;
            }
        }

        try
        {
            ResetLane(lane);
        }
        finally
        {
            lane.Gate.Release();
        }
        return true;
    }
}
