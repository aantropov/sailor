using System.Collections.ObjectModel;
using SailorEditor.Commands;

namespace SailorEditor.AI;

public sealed class AIOperatorService
{
    readonly IAIActionPlanner _planner;
    readonly IAIEditorContextProvider _aiContextProvider;
    readonly IActionContextProvider _actionContextProvider;
    readonly ICommandDispatcher _dispatcher;
    readonly Dictionary<string, AIProposedAction> _proposalLookup = new();

    public AIOperatorService(
        IAIActionPlanner planner,
        IAIEditorContextProvider aiContextProvider,
        IActionContextProvider actionContextProvider,
        ICommandDispatcher dispatcher)
    {
        _planner = planner;
        _aiContextProvider = aiContextProvider;
        _actionContextProvider = actionContextProvider;
        _dispatcher = dispatcher;
    }

    public ObservableCollection<AIConversationEntry> Conversation { get; } = new();

    public ObservableCollection<AIProposedAction> Proposals { get; } = new();

    public ObservableCollection<AIActionAuditEntry> AuditTrail { get; } = new();

    public AIEditorContextSnapshot CurrentContext => _aiContextProvider.GetCurrentContext();

    public async Task<AIPlanResponse> SubmitPromptAsync(string prompt, CancellationToken cancellationToken = default)
    {
        var trimmed = prompt?.Trim();
        if (string.IsNullOrWhiteSpace(trimmed))
            return new AIPlanResponse(string.Empty, "Tell me what to do in the editor.", CurrentContext, Array.Empty<AIProposedAction>());

        var context = _aiContextProvider.GetCurrentContext();
        Conversation.Add(new AIConversationEntry("user", trimmed, DateTimeOffset.UtcNow));

        var response = await _planner.PlanAsync(trimmed, context, cancellationToken);
        Conversation.Add(new AIConversationEntry("assistant", response.Message, DateTimeOffset.UtcNow));

        foreach (var action in response.Actions)
        {
            var normalized = action with { State = action.RequiresApproval ? AIProposalState.ApprovalRequired : AIProposalState.Draft };
            _proposalLookup[normalized.Id] = normalized;
            Proposals.Insert(0, normalized);
        }

        return response;
    }

    public bool TryApprove(string proposalId)
    {
        if (!TryGetProposal(proposalId, out var proposal) || !proposal.RequiresApproval)
            return false;

        return ReplaceProposal(proposal with { State = AIProposalState.Approved });
    }

    public bool Reject(string proposalId)
    {
        if (!TryGetProposal(proposalId, out var proposal))
            return false;

        var updated = proposal with { State = AIProposalState.Rejected };
        ReplaceProposal(updated);
        AddAudit(updated, Array.Empty<AIActionExecutionItem>(), updated.Summary);
        return true;
    }

    public async Task<AIActionAuditEntry> ExecuteProposalAsync(string proposalId, bool approved = false, CancellationToken cancellationToken = default)
    {
        if (!TryGetProposal(proposalId, out var proposal))
            return AddAudit(new AIProposedAction(proposalId, "Unknown proposal", Array.Empty<IEditorCommand>(), AIActionSafety.Elevated, "Proposal not found", AIProposalState.Failed), Array.Empty<AIActionExecutionItem>(), "Proposal not found");

        if (proposal.RequiresApproval && proposal.State != AIProposalState.Approved)
        {
            if (!approved)
                return AddAudit(proposal with { State = AIProposalState.Failed }, Array.Empty<AIActionExecutionItem>(), "Approval required before execution.");

            proposal = proposal with { State = AIProposalState.Approved };
            ReplaceProposal(proposal);
        }

        proposal = proposal with { State = AIProposalState.Executing };
        ReplaceProposal(proposal);

        var items = new List<AIActionExecutionItem>(proposal.Commands.Count);
        var metadata = new Dictionary<string, string?>
        {
            ["proposalId"] = proposal.Id,
            ["proposalTitle"] = proposal.Title,
            ["proposalSafety"] = proposal.Safety.ToString(),
        };

        foreach (var command in proposal.Commands)
        {
            var context = _actionContextProvider.GetCurrentContext(new CommandOrigin(CommandOriginKind.AI, "AIOperator", "AI Operator v1"), metadata);
            var result = await _dispatcher.DispatchAsync(command, context, cancellationToken);
            items.Add(new AIActionExecutionItem(command.Name, result.Succeeded, result.Message));
            if (!result.Succeeded)
            {
                proposal = proposal with { State = AIProposalState.Failed };
                ReplaceProposal(proposal);
                return AddAudit(proposal, items, result.Message ?? proposal.Summary);
            }
        }

        proposal = proposal with { State = AIProposalState.Executed };
        ReplaceProposal(proposal);
        return AddAudit(proposal, items, proposal.Summary);
    }

    bool TryGetProposal(string proposalId, out AIProposedAction proposal) => _proposalLookup.TryGetValue(proposalId, out proposal!);

    bool ReplaceProposal(AIProposedAction proposal)
    {
        _proposalLookup[proposal.Id] = proposal;
        for (var i = 0; i < Proposals.Count; i++)
        {
            if (Proposals[i].Id != proposal.Id)
                continue;

            Proposals[i] = proposal;
            return true;
        }

        Proposals.Insert(0, proposal);
        return true;
    }

    AIActionAuditEntry AddAudit(AIProposedAction proposal, IReadOnlyList<AIActionExecutionItem> items, string? summary)
    {
        var audit = new AIActionAuditEntry(proposal.Id, proposal.Title, proposal.Safety, proposal.State, DateTimeOffset.UtcNow, items, summary);
        AuditTrail.Insert(0, audit);
        return audit;
    }
}
