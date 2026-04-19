using SailorEditor.Panels;

namespace SailorEditor.State;

public sealed class ShellState
{
    public ShellFocusState Focus { get; private set; } = new();

    public void FocusPanel(PanelId panelId, string? groupId = null, PanelId? activeDocument = null)
    {
        Focus = Focus with
        {
            FocusedPanelId = panelId,
            ActiveTabGroupId = groupId ?? Focus.ActiveTabGroupId,
            ActiveDocumentPanelId = activeDocument ?? Focus.ActiveDocumentPanelId
        };
    }

    public void FocusViewport(string? viewportId, string? selectionOwner = null, string? keyboardInputOwner = null)
    {
        Focus = Focus with
        {
            FocusedViewportId = viewportId,
            SelectionOwner = selectionOwner,
            KeyboardInputOwner = keyboardInputOwner
        };
    }
}
