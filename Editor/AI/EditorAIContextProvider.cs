using SailorEditor.Commands;

namespace SailorEditor.AI;

public sealed class EditorAIContextProvider(IActionContextProvider actionContextProvider) : IAIEditorContextProvider
{
    public AIEditorContextSnapshot GetCurrentContext()
    {
        var context = actionContextProvider.GetCurrentContext();
        var metadata = new Dictionary<string, string?>
        {
            ["originKind"] = context.Origin?.Kind.ToString(),
            ["originSource"] = context.Origin?.Source,
            ["originActor"] = context.Origin?.Actor,
        };

        return new AIEditorContextSnapshot(
            context.ActiveWorldId,
            context.ActivePanelId,
            context.ActiveDocumentId,
            context.FocusedViewportId,
            context.CurrentAssetId,
            context.IsPlayMode,
            context.ActiveSelectionIds?.Count ?? 0,
            context.ActiveSelectionIds?.ToArray() ?? Array.Empty<string>(),
            metadata);
    }
}
