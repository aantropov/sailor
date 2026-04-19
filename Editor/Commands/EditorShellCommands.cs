using SailorEditor.Panels;
using SailorEditor.Shell;

namespace SailorEditor.Commands;

public sealed class OpenPanelCommand(EditorShellHost shellHost, PanelTypeId panelTypeId, string? title = null) : IEditorCommand
{
    public string Name => nameof(OpenPanelCommand);

    public bool CanExecute(ActionContext context) => !panelTypeId.IsEmpty;

    public async Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        await shellHost.OpenPanelAsync(panelTypeId, cancellationToken);
        return CommandResult.Success(title ?? $"Opened {panelTypeId.Value}");
    }
}
