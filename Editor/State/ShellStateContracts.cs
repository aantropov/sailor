using SailorEditor.Panels;

namespace SailorEditor.State;

public sealed record ShellFocusState(
    PanelId? FocusedPanelId = null,
    string? ActiveTabGroupId = null,
    PanelId? ActiveDocumentPanelId = null,
    string? FocusedViewportId = null,
    string? SelectionOwner = null,
    string? KeyboardInputOwner = null);
