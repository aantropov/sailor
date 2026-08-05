using SailorEditor.ViewModels;
using SailorEditor.Content;

namespace SailorEditor.Commands;

public sealed class EditAnimationControllerCommand(
    AnimationControllerFile controller,
    string before,
    string after,
    string description) : IUndoableEditorCommand
{
    public string Name => nameof(EditAnimationControllerCommand);
    public string Description => description;

    public bool CanExecute(ActionContext context) =>
        controller is not null && !controller.IsReadOnly &&
        !string.IsNullOrWhiteSpace(before) &&
        !string.IsNullOrWhiteSpace(after);

    public Task<CommandResult> ExecuteAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return Task.FromResult(Apply(after));
    }

    public ValueTask<CommandResult> UndoAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(Apply(before));
    }

    CommandResult Apply(string document)
    {
        try
        {
            if (string.Equals(
                    controller.CaptureDocument(),
                    document,
                    StringComparison.Ordinal) ||
                controller.ApplyDocument(document))
            {
                return CommandResult.Success();
            }
            return CommandResult.Failure("Animation controller document is invalid.");
        }
        catch (Exception exception)
        {
            return CommandResult.Failure(exception.Message);
        }
    }
}

public sealed class CreateAnimationAssetCommand(
    AssetFolder? folder,
    bool createSet) : IEditorCommand
{
    public string Name => nameof(CreateAnimationAssetCommand);

    public bool CanExecute(ActionContext context) =>
        MauiProgram.GetService<Services.AssetsService>().CanCreateFolder(folder);

    public async Task<CommandResult> ExecuteAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
    {
        var asset = await MauiProgram.GetService<Services.AssetsService>()
            .CreateAnimationAssetAsync(folder, createSet, cancellationToken);
        return asset is null
            ? CommandResult.Failure($"Unable to create animation {(createSet ? "set" : "controller")}.")
            : CommandResult.Success(value: asset.FileId);
    }
}
