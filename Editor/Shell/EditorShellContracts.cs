using SailorEditor.Layout;
using SailorEditor.Panels;
using SailorEditor.State;

namespace SailorEditor.Shell;

public interface IEditorShellLayoutStore
{
    ValueTask<EditorLayout?> LoadAsync(CancellationToken cancellationToken = default);

    ValueTask SaveAsync(EditorLayout layout, CancellationToken cancellationToken = default);
}

public interface IEditorShellHost
{
    ShellFocusState Focus { get; }

    EditorLayout? CurrentLayout { get; }

    ValueTask OpenPanelAsync(PanelTypeId panelTypeId, CancellationToken cancellationToken = default);

    ValueTask FocusPanelAsync(PanelId panelId, CancellationToken cancellationToken = default);
}
