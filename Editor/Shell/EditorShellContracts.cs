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

public sealed class SplitResizeDragState
{
    public double Translation { get; private set; }

    public void Begin()
        => Translation = 0;

    public double Update(double translation)
    {
        if (double.IsFinite(translation))
            Translation = translation;
        return Translation;
    }

    public double Complete()
    {
        var translation = Translation;
        Translation = 0;
        return translation;
    }

    public void Cancel()
        => Translation = 0;
}
