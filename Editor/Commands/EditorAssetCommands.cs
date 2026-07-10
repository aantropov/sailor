using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEngine;

namespace SailorEditor.Commands;

public sealed class OpenAssetCommand(AssetFile assetFile) : IEditorCommand
{
    public string Name => nameof(OpenAssetCommand);
    public bool CanExecute(ActionContext context) => assetFile?.FileId is not null && !assetFile.FileId.IsEmpty();

    public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        MauiProgram.GetService<SelectionService>().SelectObject(assetFile, force: true);
        return Task.FromResult(CommandResult.Success(value: assetFile.FileId));
    }
}

public sealed class RenameAssetCommand(AssetFile assetFile, string newName) : IEditorCommand
{
    public string Name => nameof(RenameAssetCommand);
    public bool CanExecute(ActionContext context) => assetFile is not null && !assetFile.IsReadOnly && !string.IsNullOrWhiteSpace(newName);

    public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        var success = MauiProgram.GetService<AssetsService>().RenameAsset(assetFile, newName);
        if (success)
            MauiProgram.GetService<SelectionService>().ClearSelection();
        return Task.FromResult(success ? CommandResult.Success(value: assetFile.FileId) : CommandResult.Failure("Rename failed"));
    }
}

public sealed class DeleteAssetCommand(AssetFile assetFile) : IEditorCommand
{
    public string Name => nameof(DeleteAssetCommand);
    public bool CanExecute(ActionContext context) => assetFile is { IsReadOnly: false, AssetInfo.Exists: true };

    public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        var success = MauiProgram.GetService<AssetsService>().DeleteAsset(assetFile);
        if (success)
        {
            MauiProgram.GetService<SelectionService>().ClearSelection();
            return Task.FromResult(CommandResult.Success());
        }

        return Task.FromResult(CommandResult.Failure("Delete failed"));
    }
}

public sealed class CreatePrefabAssetCommand(GameObject gameObject, AssetFolder? targetFolder = null, PrefabFile? existingPrefab = null) : IEditorCommand
{
    public string Name => nameof(CreatePrefabAssetCommand);
    public bool CanExecute(ActionContext context) => gameObject is not null
        && targetFolder?.IsReadOnly != true
        && existingPrefab?.IsReadOnly != true;

    public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        var prefab = MauiProgram.GetService<AssetsService>().CreatePrefabAsset(targetFolder, gameObject, overwrite: existingPrefab is not null, existingPrefab: existingPrefab);
        return Task.FromResult(prefab is not null
            ? CommandResult.Success(value: prefab.FileId)
            : CommandResult.Failure("Prefab asset creation failed"));
    }
}

public sealed class InstantiatePrefabAssetCommand(AssetFile prefabFile, GameObject? parent = null) : IEditorCommand
{
    readonly FileId prefabFileId = prefabFile?.FileId;

    public string Name => nameof(InstantiatePrefabAssetCommand);
    public bool CanExecute(ActionContext context) => prefabFileId is not null && !prefabFileId.IsEmpty();

    public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        var ok = MauiProgram.GetService<EngineService>().InstantiatePrefab(prefabFileId, parent?.InstanceId);
        return Task.FromResult(ok ? CommandResult.Success(value: prefabFileId) : CommandResult.Failure("Instantiate prefab failed"));
    }
}
