using SailorEditor.Mcp;
using SailorEditor.McpBridge;

if (!McpBridgeOptions.TryParse(args, out var options, out var error))
{
    await Console.Error.WriteLineAsync(error);
    return error == McpBridgeOptions.Usage ? 0 : 1;
}

var discovery = new McpEndpointDiscovery(options.DiscoveryDirectory);
var runner = new McpBridgeRunner(discovery);
return await runner.RunAsync(
    options,
    Console.OpenStandardInput(),
    Console.OpenStandardOutput(),
    Console.Error);
