using SailorEditor.Commands;

namespace SailorEditor.AI;

public enum AIActionSafety
{
    SafeAutoExecute,
    ConfirmRequired,
    Elevated,
}

public enum AIProposalState
{
    Draft,
    ApprovalRequired,
    Approved,
    Executing,
    Executed,
    Failed,
    Rejected,
}

public sealed record AIEditorContextSnapshot(
    string? ActiveWorldId,
    string? ActivePanelId,
    string? ActiveDocumentId,
    string? FocusedViewportId,
    string? CurrentAssetId,
    bool IsPlayMode,
    int SelectionCount,
    IReadOnlyList<string> SelectionIds,
    IReadOnlyDictionary<string, string?> Metadata)
{
    public static AIEditorContextSnapshot Empty { get; } = new(null, null, null, null, null, false, 0, Array.Empty<string>(), new Dictionary<string, string?>());
}

public sealed record AIProposedAction(
    string Id,
    string Title,
    IReadOnlyList<IEditorCommand> Commands,
    AIActionSafety Safety,
    string? Summary = null,
    AIProposalState State = AIProposalState.Draft)
{
    public bool RequiresApproval => Safety != AIActionSafety.SafeAutoExecute;
}

public sealed record AIPlanResponse(
    string Prompt,
    string Message,
    AIEditorContextSnapshot Context,
    IReadOnlyList<AIProposedAction> Actions);

public sealed record AIActionExecutionItem(
    string CommandName,
    bool Succeeded,
    string? Message = null);

public sealed record AIActionAuditEntry(
    string ProposalId,
    string Title,
    AIActionSafety Safety,
    AIProposalState State,
    DateTimeOffset Timestamp,
    IReadOnlyList<AIActionExecutionItem> Items,
    string? Summary = null);

public sealed record AIConversationEntry(
    string Role,
    string Text,
    DateTimeOffset Timestamp);

public interface IAIEditorContextProvider
{
    AIEditorContextSnapshot GetCurrentContext();
}

public interface IAIAgentInstructionsProvider
{
    string Instructions { get; }
}

public interface IAIActionPlanner
{
    Task<AIPlanResponse> PlanAsync(string prompt, AIEditorContextSnapshot context, CancellationToken cancellationToken = default);
}
