#nullable enable

using SailorEditor.AI;
using SailorEditor.Commands;
using SailorEditor.Workspace;

namespace SailorEditor.Mcp;

internal sealed class McpWorkspaceOperations
{
    readonly ICommandDispatcher _dispatcher;
    readonly IActionContextProvider _contextProvider;
    readonly AIOperatorService _aiOperator;

    public McpWorkspaceOperations(
        ICommandDispatcher dispatcher,
        IActionContextProvider contextProvider,
        AIOperatorService aiOperator)
    {
        _dispatcher = dispatcher;
        _contextProvider = contextProvider;
        _aiOperator = aiOperator;
    }

    public Task<CommandResult> SaveSceneAsync(
        bool confirm,
        CancellationToken cancellationToken = default) =>
        DispatchAsync(
            confirm,
            new SaveCurrentSceneCommand(),
            "MCP save scene",
            cancellationToken);

    public Task<CommandResult> BuildWorkspaceAsync(
        bool confirm,
        string configuration,
        bool configure,
        CancellationToken cancellationToken = default) =>
        DispatchAsync(
            confirm,
            new BuildWorkspaceCommand(configuration, configure),
            "MCP workspace build",
            cancellationToken);

    async Task<CommandResult> DispatchAsync(
        bool confirm,
        IEditorCommand command,
        string title,
        CancellationToken cancellationToken)
    {
        if (!confirm)
            return CommandResult.Failure("This operation requires confirm=true.");

        var context = _contextProvider.GetCurrentContext(
            new CommandOrigin(CommandOriginKind.AI, "MCP", "External MCP Agent"),
            new Dictionary<string, string?> { ["mcp"] = "true" });
        var result = await _dispatcher.DispatchAsync(
            command,
            context,
            cancellationToken);
        _aiOperator.RecordExternalExecution(
            title,
            AIActionSafety.Elevated,
            result.Succeeded ? AIProposalState.Executed : AIProposalState.Failed,
            [new AIActionExecutionItem(command.Name, result.Succeeded, result.Message)],
            result.Message);
        return result;
    }
}
