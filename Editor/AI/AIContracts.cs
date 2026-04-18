using SailorEditor.Commands;

namespace SailorEditor.AI;

public enum AIActionSafety
{
    SafeAutoExecute,
    ConfirmRequired,
    Elevated,
}

public sealed record AIProposedAction(
    string Title,
    IReadOnlyList<IEditorCommand> Commands,
    AIActionSafety Safety,
    string? Summary = null);

public interface IAIEditorContextProvider
{
    ActionContext GetCurrentContext();
}
