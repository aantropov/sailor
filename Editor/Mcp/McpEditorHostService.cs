#nullable enable

using System.Collections.Concurrent;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using ModelContextProtocol.Protocol;
using ModelContextProtocol.Server;
using SailorEditor.Services;

namespace SailorEditor.Mcp;

public sealed record McpEditorHostStatus(
    bool IsRunning,
    int ProcessId,
    int? Port,
    int ConnectedClients,
    string? DiscoveryFile,
    string? Error);

internal sealed class McpEditorHostService : IAsyncDisposable
{
    readonly McpEndpointDiscovery _discovery;
    readonly McpEditorTools _tools;
    readonly WorkspaceUiService _workspaceUi;
    readonly ConcurrentDictionary<int, Task> _clientTasks = new();
    readonly SemaphoreSlim _lifecycleGate = new(1, 1);

    CancellationTokenSource? _lifetimeCancellation;
    TcpListener? _listener;
    Task? _acceptLoop;
    McpEndpointDescriptor? _endpoint;
    int _nextClientId;
    int _connectedClients;
    bool _disposed;

    public McpEditorHostService(
        McpEndpointDiscovery discovery,
        McpEditorTools tools,
        WorkspaceUiService workspaceUi)
    {
        _discovery = discovery;
        _tools = tools;
        _workspaceUi = workspaceUi;
        Status = new McpEditorHostStatus(
            false,
            Environment.ProcessId,
            null,
            0,
            null,
            null);
        _workspaceUi.ProjectionChanged += OnWorkspaceProjectionChanged;
    }

    public McpEditorHostStatus Status { get; private set; }

    public event EventHandler? StatusChanged;

    public async Task StartAsync(CancellationToken cancellationToken = default)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        await _lifecycleGate.WaitAsync(cancellationToken);
        TcpListener? pendingListener = null;
        try
        {
            if (_listener is not null)
                return;

            var listener = new TcpListener(IPAddress.Loopback, 0);
            pendingListener = listener;
            listener.Start();
            var port = ((IPEndPoint)listener.LocalEndpoint).Port;
            var endpoint = CreateEndpoint(port);
            await _discovery.WriteAsync(endpoint, cancellationToken);

            var lifetimeCancellation = new CancellationTokenSource();
            _listener = listener;
            _endpoint = endpoint;
            _lifetimeCancellation = lifetimeCancellation;
            _acceptLoop = AcceptLoopAsync(listener, lifetimeCancellation.Token);
            pendingListener = null;
            PublishStatus(null);
        }
        catch (Exception exception)
        {
            pendingListener?.Stop();
            _listener?.Stop();
            _listener = null;
            _endpoint = null;
            _discovery.Delete(Environment.ProcessId);
            PublishStatus(exception.Message);
            throw;
        }
        finally
        {
            _lifecycleGate.Release();
        }
    }

    public async Task StopAsync(CancellationToken cancellationToken = default)
    {
        await _lifecycleGate.WaitAsync(cancellationToken);
        try
        {
            var listener = _listener;
            var lifetimeCancellation = _lifetimeCancellation;
            var acceptLoop = _acceptLoop;
            _listener = null;
            _endpoint = null;
            _lifetimeCancellation = null;
            _acceptLoop = null;

            if (listener is null)
                return;

            await lifetimeCancellation!.CancelAsync();
            listener.Stop();
            if (acceptLoop is not null)
            {
                try
                {
                    await acceptLoop.WaitAsync(cancellationToken);
                }
                catch (OperationCanceledException) when (lifetimeCancellation.IsCancellationRequested)
                {
                }
                catch (SocketException) when (lifetimeCancellation.IsCancellationRequested)
                {
                }
            }

            var clients = _clientTasks.Values.ToArray();
            if (clients.Length > 0)
            {
                try
                {
                    await Task.WhenAll(clients).WaitAsync(cancellationToken);
                }
                catch (OperationCanceledException) when (lifetimeCancellation.IsCancellationRequested)
                {
                }
            }

            lifetimeCancellation.Dispose();
            _discovery.Delete(Environment.ProcessId);
            PublishStatus(null);
        }
        finally
        {
            _lifecycleGate.Release();
        }
    }

    async Task AcceptLoopAsync(
        TcpListener listener,
        CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            TcpClient client;
            try
            {
                client = await listener.AcceptTcpClientAsync(cancellationToken);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                break;
            }
            catch (SocketException) when (cancellationToken.IsCancellationRequested)
            {
                break;
            }

            var clientId = Interlocked.Increment(ref _nextClientId);
            var task = HandleClientSafelyAsync(client, cancellationToken);
            _clientTasks[clientId] = task;
            _ = task.ContinueWith(
                completedTask => _clientTasks.TryRemove(clientId, out _),
                CancellationToken.None,
                TaskContinuationOptions.ExecuteSynchronously,
                TaskScheduler.Default);
        }
    }

    async Task HandleClientSafelyAsync(
        TcpClient client,
        CancellationToken cancellationToken)
    {
        try
        {
            await HandleClientAsync(client, cancellationToken);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(
                "Sailor Editor MCP client connection failed: " +
                exception.Message);
        }
    }

    async Task HandleClientAsync(
        TcpClient client,
        CancellationToken cancellationToken)
    {
        using (client)
        await using (var stream = client.GetStream())
        {
            var endpoint = _endpoint;
            if (endpoint is null)
            {
                return;
            }

            using var handshakeCancellation =
                CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            handshakeCancellation.CancelAfter(TimeSpan.FromSeconds(10));
            try
            {
                if (!await McpLoopbackAuthentication.ValidateRequestAsync(
                        stream,
                        endpoint.AuthenticationToken,
                        handshakeCancellation.Token))
                {
                    return;
                }
            }
            catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
            {
                return;
            }

            Interlocked.Increment(ref _connectedClients);
            PublishStatus(null);
            try
            {
                await using var server = McpServer.Create(
                    new StreamServerTransport(
                        stream,
                        stream,
                        $"SailorEditor-{Environment.ProcessId}"),
                    new McpServerOptions
                    {
                        ServerInfo = new Implementation
                        {
                            Name = "SailorEditor",
                            Version = "1.0.0",
                        },
                        ToolCollection = [.. _tools.CreateTools()],
                    });
                await server.RunAsync(cancellationToken);
            }
            finally
            {
                Interlocked.Decrement(ref _connectedClients);
                PublishStatus(null);
            }
        }
    }

    McpEndpointDescriptor CreateEndpoint(int port)
    {
        var projection = _workspaceUi.Projection;
        return new McpEndpointDescriptor(
            McpEndpointDiscovery.CurrentProtocolVersion,
            Process.GetCurrentProcess().Id,
            port,
            McpEndpointDiscovery.CreateAuthenticationToken(),
            projection.Mode.ToString(),
            projection.HasActiveWorkspace
                ? projection.ActiveWorkspacePath
                : projection.ActiveRootPath,
            DateTimeOffset.UtcNow);
    }

    void OnWorkspaceProjectionChanged(object? sender, EventArgs eventArgs)
    {
        _ = RefreshEndpointAsync();
    }

    async Task RefreshEndpointAsync()
    {
        await _lifecycleGate.WaitAsync();
        try
        {
            var endpoint = _endpoint;
            if (endpoint is null)
                return;

            var projection = _workspaceUi.Projection;
            var updated = endpoint with
            {
                ProjectMode = projection.Mode.ToString(),
                WorkspacePath = projection.HasActiveWorkspace
                    ? projection.ActiveWorkspacePath
                    : projection.ActiveRootPath,
            };
            _endpoint = updated;
            await _discovery.WriteAsync(updated);
            PublishStatus(null);
        }
        catch (Exception exception)
        {
            PublishStatus(exception.Message);
        }
        finally
        {
            _lifecycleGate.Release();
        }
    }

    void PublishStatus(string? error)
    {
        var endpoint = _endpoint;
        Status = new McpEditorHostStatus(
            endpoint is not null,
            Environment.ProcessId,
            endpoint?.Port,
            Volatile.Read(ref _connectedClients),
            endpoint is null
                ? null
                : _discovery.GetDescriptorPath(endpoint.ProcessId),
            error);
        StatusChanged?.Invoke(this, EventArgs.Empty);
    }

    public async ValueTask DisposeAsync()
    {
        if (_disposed)
            return;

        _disposed = true;
        _workspaceUi.ProjectionChanged -= OnWorkspaceProjectionChanged;
        await StopAsync();
        GC.SuppressFinalize(this);
    }
}
