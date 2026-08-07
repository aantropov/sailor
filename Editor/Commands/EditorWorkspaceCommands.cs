#nullable enable

using SailorEditor.Services;
using SailorEditor.Workspace;

namespace SailorEditor.Commands;

public sealed class BuildWorkspaceCommand(
    string configuration = "Release",
    bool configure = true) : IEditorCommand
{
    public string Name => nameof(BuildWorkspaceCommand);
    public bool CanExecute(ActionContext context) =>
        MauiProgram.GetService<WorkspaceLifecycleService>().Current is not null;

    public async Task<CommandResult> ExecuteAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
    {
        var result = await MauiProgram.GetService<WorkspaceBuildService>()
            .BuildAsync(configuration, configure, cancellationToken);
        return result.Succeeded
            ? CommandResult.Success("Workspace build completed.", result)
            : CommandResult.Failure(
                result.Error ?? "Workspace build failed.",
                result);
    }
}

public sealed class SaveCurrentSceneCommand : IEditorCommand
{
    public string Name => nameof(SaveCurrentSceneCommand);
    public bool CanExecute(ActionContext context) => true;

    public async Task<CommandResult> ExecuteAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
    {
        var result = await MauiProgram.GetService<WorldService>()
            .SaveCurrentWorldAsync(
                confirmExisting: false,
                cancellationToken);
        return result.Outcome == SceneSaveOutcome.Saved
            ? CommandResult.Success(
                $"Scene saved to '{result.Path}'.",
                result)
            : CommandResult.Failure(
                result.Error ??
                    (result.Outcome == SceneSaveOutcome.Cancelled
                        ? "Scene save was cancelled."
                        : "Scene save failed."),
                result);
    }
}
