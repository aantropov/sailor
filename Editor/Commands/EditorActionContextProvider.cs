using SailorEditor.Services;
using SailorEditor.State;
using SailorEditor.ViewModels;

namespace SailorEditor.Commands;

public sealed class EditorActionContextProvider(
    WorldService worldService,
    SelectionService selectionService,
    ShellState shellState) : IActionContextProvider
{
    public ActionContext GetCurrentContext(
        CommandOrigin? origin = null,
        IReadOnlyDictionary<string, string?>? metadata = null)
    {
        var selectedIds = selectionService.SelectedItems
            .Select(TryGetSelectionId)
            .Where(id => !string.IsNullOrWhiteSpace(id))
            .Cast<string>()
            .ToArray();

        return new ActionContext(
            ActiveWorldId: worldService.Current?.Name,
            ActiveSelectionIds: selectedIds,
            ActivePanelId: shellState.Focus.FocusedPanelId?.Value,
            ActiveDocumentId: shellState.Focus.ActiveDocumentPanelId?.Value,
            FocusedViewportId: shellState.Focus.FocusedViewportId,
            CurrentAssetId: TryGetSelectionId(selectionService.SelectedItem),
            IsPlayMode: false,
            Origin: origin,
            Metadata: metadata,
            WorkspaceEpoch: selectionService.WorkspaceEpoch);
    }

    static string? TryGetSelectionId(object? selected) => selected switch
    {
        GameObject gameObject => gameObject.InstanceId?.Value,
        Component component => component.InstanceId?.Value,
        _ => null,
    };
}
