using System.Net;
using System.Net.Sockets;
using SailorEditor.Mcp;

namespace SailorEditor.McpBridge;

public sealed class McpBridgeRunner
{
    readonly McpEndpointDiscovery _discovery;

    public McpBridgeRunner(McpEndpointDiscovery discovery)
    {
        _discovery = discovery;
    }

    public async Task<int> RunAsync(
        McpBridgeOptions options,
        Stream standardInput,
        Stream standardOutput,
        TextWriter standardError,
        CancellationToken cancellationToken = default)
    {
        var selection = _discovery.Select(options.ProcessId, options.WorkspacePath);
        if (!selection.Succeeded)
        {
            await standardError.WriteLineAsync(selection.Error);
            return 2;
        }

        var endpoint = selection.Endpoint!;
        using var client = new TcpClient(AddressFamily.InterNetwork);
        try
        {
            await client.ConnectAsync(
                IPAddress.Loopback,
                endpoint.Port,
                cancellationToken);
        }
        catch (Exception exception) when (exception is SocketException or IOException)
        {
            await standardError.WriteLineAsync(
                $"Unable to connect to Sailor Editor PID {endpoint.ProcessId}: {exception.Message}");
            return 3;
        }

        await using var stream = client.GetStream();
        await McpLoopbackAuthentication.WriteRequestAsync(
            stream,
            endpoint.AuthenticationToken,
            cancellationToken);

        using var relayCancellation =
            CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        var inputRelay = standardInput.CopyToAsync(stream, relayCancellation.Token);
        var outputRelay = stream.CopyToAsync(standardOutput, relayCancellation.Token);
        var completed = await Task.WhenAny(inputRelay, outputRelay);
        await completed;
        await relayCancellation.CancelAsync();

        try
        {
            await Task.WhenAll(inputRelay, outputRelay);
        }
        catch (OperationCanceledException) when (relayCancellation.IsCancellationRequested)
        {
        }

        return 0;
    }
}
