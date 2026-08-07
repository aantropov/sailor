using System.IO.Pipelines;
using System.Net;
using System.Net.Sockets;
using ModelContextProtocol.Client;
using ModelContextProtocol.Protocol;
using ModelContextProtocol.Server;
using SailorEditor.Mcp;
using SailorEditor.McpBridge;

namespace SailorEditor.Tests;

public sealed class McpBridgeIntegrationTests
{
    [Fact]
    public async Task Bridge_ConnectsOfficialClientToAuthenticatedEditorServer()
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(15));
        var directory = Path.Combine(
            Path.GetTempPath(),
            "sailor-mcp-bridge-tests-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        var discovery = new McpEndpointDiscovery(directory);
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var port = ((IPEndPoint)listener.LocalEndpoint).Port;
        var token = McpEndpointDiscovery.CreateAuthenticationToken();
        await discovery.WriteAsync(new McpEndpointDescriptor(
            McpEndpointDiscovery.CurrentProtocolVersion,
            Environment.ProcessId,
            port,
            token,
            "Engine",
            directory,
            DateTimeOffset.UtcNow), timeout.Token);

        var serverTask = RunServerAsync(listener, token, timeout.Token);
        var clientToBridge = new Pipe();
        var bridgeToClient = new Pipe();
        var bridge = new McpBridgeRunner(discovery);
        var bridgeTask = bridge.RunAsync(
            new McpBridgeOptions(Environment.ProcessId, null, directory),
            clientToBridge.Reader.AsStream(),
            bridgeToClient.Writer.AsStream(),
            TextWriter.Null,
            timeout.Token);

        try
        {
            await using var client = await McpClient.CreateAsync(
                new StreamClientTransport(
                    clientToBridge.Writer.AsStream(),
                    bridgeToClient.Reader.AsStream()),
                cancellationToken: timeout.Token);
            var tools = await client.ListToolsAsync(
                cancellationToken: timeout.Token);

            Assert.Contains(tools, tool => tool.Name == "sailor_test_ping");
        }
        finally
        {
            await clientToBridge.Writer.CompleteAsync();
            await bridgeToClient.Reader.CompleteAsync();
            listener.Stop();
            timeout.Cancel();
            try
            {
                await bridgeTask;
            }
            catch (OperationCanceledException)
            {
            }
            try
            {
                await serverTask;
            }
            catch (OperationCanceledException)
            {
            }
            catch (SocketException)
            {
            }
            discovery.Delete(Environment.ProcessId);
            Directory.Delete(directory, recursive: true);
        }
    }

    static async Task RunServerAsync(
        TcpListener listener,
        string token,
        CancellationToken cancellationToken)
    {
        using var connection = await listener.AcceptTcpClientAsync(cancellationToken);
        await using var stream = connection.GetStream();
        Assert.True(await McpLoopbackAuthentication.ValidateRequestAsync(
            stream,
            token,
            cancellationToken));

        await using var server = McpServer.Create(
            new StreamServerTransport(stream, stream, "SailorEditorTest"),
            new McpServerOptions
            {
                ServerInfo = new Implementation
                {
                    Name = "SailorEditorTest",
                    Version = "1.0.0",
                },
                ToolCollection =
                [
                    McpServerTool.Create(
                        () => "pong",
                        new() { Name = "sailor_test_ping" }),
                ],
            });
        await server.RunAsync(cancellationToken);
    }
}
