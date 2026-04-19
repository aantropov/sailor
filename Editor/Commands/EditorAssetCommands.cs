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
    public bool CanExecute(ActionContext context) => assetFile is not null && !string.IsNullOrWhiteSpace(newName);

    public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        var success = MauiProgram.GetService<AssetsService>().RenameAsset(assetFile, newName);
        return Task.FromResult(success ? CommandResult.Success(value: assetFile.FileId) : CommandResult.Failure("Rename failed"));
    }
}

public sealed class DeleteAssetCommand(AssetFile assetFile) : IEditorCommand
{
    public string Name => nameof(DeleteAssetCommand);
    public bool CanExecute(ActionContext context) => assetFile?.AssetInfo?.Exists == true;

    public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        try
        {
            if (assetFile.Asset?.Exists == true)
                File.Delete(assetFile.Asset.FullName);
            if (assetFile.AssetInfo.Exists)
                File.Delete(assetFile.AssetInfo.FullName);

            MauiProgram.GetService<AssetsService>().AddProjectRoot(MauiProgram.GetService<EngineService>().EngineContentDirectory);
            MauiProgram.GetService<SelectionService>().ClearSelection();
            return Task.FromResult(CommandResult.Success());
        }
        catch (Exception ex)
        {
            return Task.FromResult(CommandResult.Failure(ex.Message));
        }
    }
}

public sealed class CreatePrefabAssetCommand(GameObject gameObject, AssetFolder? targetFolder = null, PrefabFile? existingPrefab = null) : IEditorCommand
{
    public string Name => nameof(CreatePrefabAssetCommand);
    public bool CanExecute(ActionContext context) => gameObject is not null;

    public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        var prefab = MauiProgram.GetService<AssetsService>().CreatePrefabAsset(targetFolder, gameObject, overwrite: existingPrefab is not null, existingPrefab: existingPrefab);
        return Task.FromResult(prefab is not null
            ? CommandResult.Success(value: prefab.FileId)
            : CommandResult.Failure("Prefab asset creation failed"));
    }
}

public sealed class InstantiatePrefabAssetCommand(PrefabFile prefabFile, GameObject? parent = null) : IEditorCommand
{
    public string Name => nameof(InstantiatePrefabAssetCommand);
    public bool CanExecute(ActionContext context) => prefabFile?.FileId is not null && !prefabFile.FileId.IsEmpty();

    public Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        var ok = MauiProgram.GetService<EngineService>().InstantiatePrefab(prefabFile.FileId, parent?.InstanceId);
        return Task.FromResult(ok ? CommandResult.Success(value: prefabFile.FileId) : CommandResult.Failure("Instantiate prefab failed"));
    }
}
