namespace SailorEditor.McpBridge;

public sealed record McpBridgeOptions(
    int? ProcessId,
    string? WorkspacePath,
    string? DiscoveryDirectory)
{
    public static bool TryParse(
        IReadOnlyList<string> arguments,
        out McpBridgeOptions options,
        out string? error)
    {
        int? processId = null;
        string? workspacePath = null;
        string? discoveryDirectory = null;

        for (var index = 0; index < arguments.Count; index++)
        {
            var argument = arguments[index];
            if (argument is "--help" or "-h")
            {
                options = new McpBridgeOptions(null, null, null);
                error = Usage;
                return false;
            }

            if (argument is not ("--pid" or "--workspace" or "--discovery-directory"))
            {
                options = new McpBridgeOptions(null, null, null);
                error = $"Unknown argument '{argument}'.\n{Usage}";
                return false;
            }

            if (++index >= arguments.Count)
            {
                options = new McpBridgeOptions(null, null, null);
                error = $"Missing value for '{argument}'.\n{Usage}";
                return false;
            }

            var value = arguments[index];
            switch (argument)
            {
                case "--pid":
                    if (!int.TryParse(value, out var parsedProcessId) || parsedProcessId <= 0)
                    {
                        options = new McpBridgeOptions(null, null, null);
                        error = "--pid must be a positive integer.";
                        return false;
                    }

                    processId = parsedProcessId;
                    break;
                case "--workspace":
                    workspacePath = value;
                    break;
                case "--discovery-directory":
                    discoveryDirectory = value;
                    break;
            }
        }

        options = new McpBridgeOptions(processId, workspacePath, discoveryDirectory);
        error = null;
        return true;
    }

    public const string Usage =
        "Usage: SailorEditor.McpBridge [--pid <editor-pid>] [--workspace <path>] " +
        "[--discovery-directory <path>]";
}
