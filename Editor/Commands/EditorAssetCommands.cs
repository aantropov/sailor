using SailorEditor.Content;
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
    public bool CanExecute(ActionContext context) => !string.IsNullOrWhiteSpace(newName)
        && MauiProgram.GetService<AssetsService>().CanRenameAsset(assetFile);

    public async Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        var operation = MauiProgram.GetService<AssetsService>().RenameAsset(assetFile, newName);
        return await AssetCommandMessages.CompleteMutationAsync(
            operation,
            "Rename failed",
            "Asset files were renamed, but the native registry rejected the reload",
            () => CommandResult.Success(value: assetFile.FileId),
            cancellationToken);
    }
}

public sealed class DeleteAssetCommand(AssetFile assetFile) : IEditorCommand
{
    public string Name => nameof(DeleteAssetCommand);
    public bool CanExecute(ActionContext context) => MauiProgram.GetService<AssetsService>().CanModifyAsset(assetFile);

    public async Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        var operation = MauiProgram.GetService<AssetsService>().DeleteAsset(assetFile);
        return await AssetCommandMessages.CompleteMutationAsync(
            operation,
            "Delete failed",
            "Asset files were deleted, but the native registry rejected the reload",
            () => CommandResult.Success(),
            cancellationToken);
    }
}

public sealed class DuplicateAssetCommand(AssetFile assetFile) : IEditorCommand
{
    public string Name => nameof(DuplicateAssetCommand);
    public bool CanExecute(ActionContext context) => MauiProgram.GetService<AssetsService>().CanDuplicateAsset(assetFile);

    public async Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        var operation = MauiProgram.GetService<AssetsService>().DuplicateAsset(assetFile);
        return await AssetCommandMessages.CompleteMutationAsync(
            operation,
            "Duplicate failed",
            "The asset group was duplicated, but the native registry rejected the reload",
            () => CommandResult.Success(value: operation.CreatedFileId),
            cancellationToken);
    }
}

public sealed class MoveAssetCommand(AssetFile assetFile, AssetFolder? destinationFolder = null) : IEditorCommand
{
    public string Name => nameof(MoveAssetCommand);
    public bool CanExecute(ActionContext context) => MauiProgram.GetService<AssetsService>().CanMoveAsset(assetFile, destinationFolder);

    public async Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        var operation = MauiProgram.GetService<AssetsService>().MoveAsset(assetFile, destinationFolder);
        return await AssetCommandMessages.CompleteMutationAsync(
            operation,
            "Move failed",
            "Asset files were moved, but the native registry rejected the reload",
            () => CommandResult.Success(value: assetFile.FileId),
            cancellationToken);
    }
}

public sealed class CreateFolderCommand(AssetFolder? parentFolder, string folderName) : IEditorCommand
{
    public string Name => nameof(CreateFolderCommand);
    public bool CanExecute(ActionContext context) => !string.IsNullOrWhiteSpace(folderName)
        && MauiProgram.GetService<AssetsService>().CanCreateFolder(parentFolder);

    public async Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        var operation = MauiProgram.GetService<AssetsService>().CreateFolder(parentFolder, folderName);
        return await AssetCommandMessages.CompleteMutationAsync(
            operation,
            "Create folder failed",
            "The folder was created, but the native registry rejected the reload",
            () => CommandResult.Success(
                value: MauiProgram.GetService<ProjectContentStore>().State.CurrentFolderId),
            cancellationToken);
    }
}

public sealed class MoveFolderCommand(AssetFolder folder, AssetFolder? destinationFolder = null) : IEditorCommand
{
    public string Name => nameof(MoveFolderCommand);
    public bool CanExecute(ActionContext context) => MauiProgram.GetService<AssetsService>().CanMoveFolder(folder, destinationFolder);

    public async Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        var operation = MauiProgram.GetService<AssetsService>().MoveFolder(folder, destinationFolder);
        return await AssetCommandMessages.CompleteMutationAsync(
            operation,
            "Move folder failed",
            "The folder was moved, but the native registry rejected the reload",
            () => CommandResult.Success(),
            cancellationToken);
    }
}

public sealed class RenameFolderCommand(AssetFolder folder, string newName) : IEditorCommand
{
    public string Name => nameof(RenameFolderCommand);
    public bool CanExecute(ActionContext context) => !string.IsNullOrWhiteSpace(newName)
        && MauiProgram.GetService<AssetsService>().CanModifyFolder(folder);

    public async Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        var operation = MauiProgram.GetService<AssetsService>().RenameFolder(folder, newName);
        return await AssetCommandMessages.CompleteMutationAsync(
            operation,
            "Rename folder failed",
            "The folder was renamed, but the native registry rejected the reload",
            () => CommandResult.Success(),
            cancellationToken);
    }
}

public sealed class DeleteFolderCommand(AssetFolder folder) : IEditorCommand
{
    public string Name => nameof(DeleteFolderCommand);
    public bool CanExecute(ActionContext context) => MauiProgram.GetService<AssetsService>().CanModifyFolder(folder);

    public async Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        var operation = MauiProgram.GetService<AssetsService>().DeleteFolder(folder);
        return await AssetCommandMessages.CompleteMutationAsync(
            operation,
            "Delete folder failed",
            "The folder was deleted, but the native registry rejected the reload",
            () => CommandResult.Success(),
            cancellationToken);
    }
}

public sealed class CreatePrefabAssetCommand(GameObject gameObject, AssetFolder? targetFolder = null, PrefabFile? existingPrefab = null) : IEditorCommand
{
    public string Name => nameof(CreatePrefabAssetCommand);
    public bool CanExecute(ActionContext context) => gameObject is not null
        && targetFolder?.IsReadOnly != true
        && existingPrefab?.IsReadOnly != true;

    public async Task<CommandResult> ExecuteAsync(ActionContext context, CancellationToken cancellationToken = default)
    {
        var prefab = MauiProgram.GetService<AssetsService>().CreatePrefabAsset(targetFolder, gameObject, overwrite: existingPrefab is not null, existingPrefab: existingPrefab);
        if (prefab is null)
            return CommandResult.Failure("Prefab asset creation failed");

        return await MauiProgram.GetService<EngineService>().RequestAssetReloadAsync(cancellationToken)
            ? CommandResult.Success(value: prefab.FileId)
            : CommandResult.Failure("Prefab files were created, but the native registry rejected the reload");
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

static class AssetCommandMessages
{
    public static async Task<CommandResult> CompleteMutationAsync(
        ProjectContentFileOperationResult operation,
        string failureFallback,
        string reloadRejectedMessage,
        Func<CommandResult> success,
        CancellationToken cancellationToken)
    {
        var reloadAccepted = await ProjectContentMutationReloadPolicy.RequestNativeReloadIfRequiredAsync(
            operation,
            token => MauiProgram.GetService<EngineService>().RequestAssetReloadAsync(token),
            cancellationToken);
        if (!operation.Succeeded)
        {
            var error = Error(operation, failureFallback);
            if (reloadAccepted == false)
                error += " The native registry rejected the reconciliation reload.";
            return CommandResult.Failure(error);
        }

        return reloadAccepted == true
            ? success()
            : CommandResult.Failure(reloadRejectedMessage);
    }

    public static string Error(ProjectContentFileOperationResult operation, string fallback)
    {
        var error = string.IsNullOrWhiteSpace(operation.Error) ? fallback : operation.Error;
        if (operation.RollbackSucceeded || operation.UnrestoredPaths.Count == 0)
            return error;

        return $"{error} Rollback was incomplete; paths requiring recovery: {string.Join(", ", operation.UnrestoredPaths)}";
    }
}
