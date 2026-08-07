using SailorEditor.Mcp;

namespace SailorEditor.Tests;

public sealed class McpEndpointDiscoveryTests
{
    [Fact]
    public async Task WriteAndSelect_RoundTripsCurrentEditorEndpoint()
    {
        var directory = CreateTemporaryDirectory();
        try
        {
            var discovery = new McpEndpointDiscovery(directory);
            var endpoint = new McpEndpointDescriptor(
                McpEndpointDiscovery.CurrentProtocolVersion,
                Environment.ProcessId,
                34567,
                McpEndpointDiscovery.CreateAuthenticationToken(),
                "Workspace",
                directory,
                DateTimeOffset.UtcNow);

            await discovery.WriteAsync(endpoint);

            var selection = discovery.Select(
                processId: Environment.ProcessId);
            Assert.True(selection.Succeeded, selection.Error);
            Assert.Equal(endpoint, selection.Endpoint);
            Assert.DoesNotContain(
                Directory.EnumerateFiles(directory),
                path => path.EndsWith(".tmp", StringComparison.Ordinal));

            if (!OperatingSystem.IsWindows())
            {
                var mode = File.GetUnixFileMode(
                    discovery.GetDescriptorPath(Environment.ProcessId));
                Assert.Equal(
                    UnixFileMode.UserRead | UnixFileMode.UserWrite,
                    mode);
            }
        }
        finally
        {
            Directory.Delete(directory, recursive: true);
        }
    }

    [Fact]
    public async Task Authentication_ConsumesOnlyHandshakeLine()
    {
        const string token = "correct-token";
        await using var stream = new MemoryStream();
        await McpLoopbackAuthentication.WriteRequestAsync(stream, token);
        await stream.WriteAsync("{\"jsonrpc\":\"2.0\"}\n"u8.ToArray());
        stream.Position = 0;

        Assert.True(await McpLoopbackAuthentication.ValidateRequestAsync(
            stream,
            token));

        using var reader = new StreamReader(stream);
        Assert.Equal("{\"jsonrpc\":\"2.0\"}", await reader.ReadLineAsync());
    }

    [Fact]
    public async Task Select_RejectsDescriptorFromReusedProcessId()
    {
        var directory = CreateTemporaryDirectory();
        try
        {
            var discovery = new McpEndpointDiscovery(directory);
            using var currentProcess = System.Diagnostics.Process.GetCurrentProcess();
            var endpoint = new McpEndpointDescriptor(
                McpEndpointDiscovery.CurrentProtocolVersion,
                Environment.ProcessId,
                34567,
                McpEndpointDiscovery.CreateAuthenticationToken(),
                "Engine",
                directory,
                currentProcess.StartTime.ToUniversalTime().AddMinutes(-5));
            await discovery.WriteAsync(endpoint);

            var selection = discovery.Select(processId: Environment.ProcessId);

            Assert.False(selection.Succeeded);
            Assert.False(File.Exists(
                discovery.GetDescriptorPath(Environment.ProcessId)));
        }
        finally
        {
            Directory.Delete(directory, recursive: true);
        }
    }

    [Fact]
    public async Task Authentication_RejectsWrongToken()
    {
        await using var stream = new MemoryStream();
        await McpLoopbackAuthentication.WriteRequestAsync(stream, "wrong");
        stream.Position = 0;

        Assert.False(await McpLoopbackAuthentication.ValidateRequestAsync(
            stream,
            "correct"));
    }

    static string CreateTemporaryDirectory()
    {
        var path = Path.Combine(
            Path.GetTempPath(),
            "sailor-mcp-tests-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(path);
        return path;
    }
}
