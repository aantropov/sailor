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

public sealed class ReimportAssetCommand(AssetFile assetFile) : IEditorCommand
{
    public string Name => nameof(ReimportAssetCommand);
    public bool CanExecute(ActionContext context) =>
        assetFile?.FileId is not null &&
        !assetFile.FileId.IsEmpty();

    public async Task<CommandResult> ExecuteAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
    {
        var updated = await MauiProgram.GetService<EngineService>()
            .UpdateAssetAsync(assetFile.FileId, cancellationToken);
        return updated
            ? CommandResult.Success(
                "Asset reimport completed.",
                assetFile.FileId)
            : CommandResult.Failure("Asset reimport failed.");
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
        cancellationToken.ThrowIfCancellationRequested();
        var atomicCancellationToken = CancellationToken.None;
        var assets = MauiProgram.GetService<AssetsService>();
        var world = MauiProgram.GetService<WorldService>();
        if (gameObject.InstanceId is null ||
            gameObject.InstanceId.IsEmpty())
        {
            return CommandResult.Failure(
                "The source GameObject is no longer available");
        }
        if (HasProjectedPrefabLink(
                world,
                gameObject.InstanceId))
        {
            return CommandResult.Failure(
                "Break the existing prefab link before creating another prefab");
        }

        var creation = assets.BeginCreatePrefabAsset(
            targetFolder,
            gameObject,
            overwrite: existingPrefab is not null,
            existingPrefab: existingPrefab);
        if (creation is null)
            return CommandResult.Failure("Prefab asset creation failed");

        var engine = MauiProgram.GetService<EngineService>();
        var linkAttempted = false;
        var linkEstablished = false;
        try
        {
            if (!await engine.RequestAssetReloadAsync(
                    atomicCancellationToken))
            {
                return await RollbackAsync(
                    assets,
                    creation,
                    engine,
                    gameObject.InstanceId,
                    cleanupPossibleLink: false,
                    "The native registry rejected the prefab reload",
                    atomicCancellationToken);
            }

            if (gameObject.InstanceId is null ||
                gameObject.InstanceId.IsEmpty() ||
                creation.Prefab.FileId is null ||
                creation.Prefab.FileId.IsEmpty())
            {
                return await RollbackAsync(
                    assets,
                    creation,
                    engine,
                    gameObject.InstanceId,
                    cleanupPossibleLink: false,
                    "The source GameObject could not be linked to the prefab",
                    atomicCancellationToken);
            }

            linkAttempted = true;
            linkEstablished = await engine.SetPrefabLinkAsync(
                gameObject.InstanceId,
                creation.Prefab.FileId,
                atomicCancellationToken);
            if (!linkEstablished)
            {
                return await RollbackAsync(
                    assets,
                    creation,
                    engine,
                    gameObject.InstanceId,
                    cleanupPossibleLink: false,
                    "The source GameObject could not be linked to the prefab",
                    atomicCancellationToken);
            }
            if (!IsProjectedLinkedPrefab(
                    world,
                    gameObject.InstanceId,
                    creation.Prefab.FileId))
            {
                return await RollbackAsync(
                    assets,
                    creation,
                    engine,
                    gameObject.InstanceId,
                    cleanupPossibleLink: true,
                    "The source GameObject prefab link was not projected",
                    atomicCancellationToken);
            }

            var commit = assets.CompletePrefabAssetWrite(
                creation.Transaction,
                commit: true);
            if (!commit.Succeeded)
            {
                return await RollbackAsync(
                    assets,
                    creation,
                    engine,
                    gameObject.InstanceId,
                    cleanupPossibleLink: true,
                    AssetCommandMessages.Error(
                        commit,
                        "The prefab write transaction could not be committed"),
                    atomicCancellationToken);
            }

            return CommandResult.Success(value: creation.Prefab.FileId);
        }
        catch (Exception exception)
        {
            return await RollbackAsync(
                assets,
                creation,
                engine,
                gameObject.InstanceId,
                cleanupPossibleLink: linkAttempted || linkEstablished,
                $"Prefab creation failed unexpectedly: {exception.Message}",
                atomicCancellationToken);
        }
    }

    static async Task<CommandResult> RollbackAsync(
        AssetsService assets,
        PrefabAssetWriteResult creation,
        EngineService engine,
        InstanceId? linkedRootInstanceId,
        bool cleanupPossibleLink,
        string reason,
        CancellationToken cancellationToken)
    {
        var diagnostics = new List<string> { reason };
        if (cleanupPossibleLink &&
            linkedRootInstanceId is not null &&
            !linkedRootInstanceId.IsEmpty())
        {
            try
            {
                if (!await engine.BreakPrefabLinkAsync(
                        linkedRootInstanceId,
                        cancellationToken))
                {
                    diagnostics.Add(
                        "The live prefab link cleanup could not be confirmed.");
                }
            }
            catch (Exception exception)
            {
                diagnostics.Add(
                    $"The live prefab link cleanup failed: {exception.Message}");
            }
        }

        try
        {
            if (creation.Transaction.IsActive)
            {
                var rollback = assets.CompletePrefabAssetWrite(
                    creation.Transaction,
                    commit: false);
                if (!rollback.Succeeded)
                {
                    diagnostics.Add(
                        AssetCommandMessages.Error(
                            rollback,
                            "Prefab file rollback failed"));
                }
            }
        }
        catch (Exception exception)
        {
            diagnostics.Add(
                $"Prefab file rollback failed: {exception.Message}");
        }

        try
        {
            if (!await engine.RequestAssetReloadAsync(
                    cancellationToken))
            {
                diagnostics.Add(
                    "The native registry rejected the rollback reconciliation reload.");
            }
        }
        catch (Exception exception)
        {
            diagnostics.Add(
                $"The rollback reconciliation reload failed: {exception.Message}");
        }
        if (cleanupPossibleLink &&
            linkedRootInstanceId is not null &&
            !linkedRootInstanceId.IsEmpty())
        {
            try
            {
                await engine.RefreshCurrentWorldAsync(
                    CancellationToken.None);
                if (HasProjectedPrefabLink(
                        MauiProgram.GetService<WorldService>(),
                        linkedRootInstanceId))
                {
                    diagnostics.Add(
                        "The live prefab link cleanup is still present in the world projection.");
                }
            }
            catch (Exception exception)
            {
                diagnostics.Add(
                    $"The world projection reconciliation failed: {exception.Message}");
            }
        }

        return CommandResult.Failure(
            string.Join(" ", diagnostics.Where(
                diagnostic => !string.IsNullOrWhiteSpace(diagnostic))));
    }

    static bool IsProjectedLinkedPrefab(
        WorldService world,
        InstanceId rootInstanceId,
        FileId prefabFileId)
    {
        if (!world.TryGetGameObject(rootInstanceId, out var root) ||
            root.PrefabIndex < 0 ||
            root.PrefabIndex >= world.Current.Prefabs.Count)
        {
            return false;
        }

        var projectedPrefab = world.Current.Prefabs[root.PrefabIndex];
        return projectedPrefab.FileId is not null &&
            projectedPrefab.FileId.Equals(prefabFileId);
    }

    static bool HasProjectedPrefabLink(
        WorldService world,
        InstanceId rootInstanceId)
    {
        if (!world.TryGetGameObject(rootInstanceId, out var root) ||
            root.PrefabIndex < 0 ||
            root.PrefabIndex >= world.Current.Prefabs.Count)
        {
            return false;
        }

        var projectedPrefab = world.Current.Prefabs[root.PrefabIndex];
        return projectedPrefab.FileId is not null &&
            !projectedPrefab.FileId.IsEmpty();
    }
}

public sealed class ApplyPrefabInstanceCommand(GameObject gameObject) : IEditorCommand
{
    readonly InstanceId _instanceId = gameObject.InstanceId is null
        ? InstanceId.NullInstanceId
        : new InstanceId(gameObject.InstanceId.Value);

    public string Name => nameof(ApplyPrefabInstanceCommand);

    public bool CanExecute(ActionContext context) =>
        _instanceId is not null &&
        !_instanceId.IsEmpty();

    public async Task<CommandResult> ExecuteAsync(
        ActionContext context,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var engine = MauiProgram.GetService<EngineService>();
        var world = MauiProgram.GetService<WorldService>();
        await engine.RefreshCurrentWorldAsync(cancellationToken);
        if (!world.TryGetGameObject(_instanceId, out var liveObject) ||
            liveObject.PrefabIndex < 0 ||
            liveObject.PrefabIndex >= world.Current.Prefabs.Count)
        {
            return CommandResult.Failure(
                "The selected GameObject is no longer available.");
        }

        var linkedPrefab = world.Current.Prefabs[liveObject.PrefabIndex];
        if (linkedPrefab.FileId is null || linkedPrefab.FileId.IsEmpty())
        {
            return CommandResult.Failure(
                "The selected GameObject is not linked to a prefab.");
        }

        var assets = MauiProgram.GetService<AssetsService>();
        if (!assets.Assets.TryGetValue(
                linkedPrefab.FileId,
                out var sourceAsset) ||
            sourceAsset is not PrefabFile sourcePrefab)
        {
            return CommandResult.Failure(
                $"Prefab asset '{linkedPrefab.FileId.Value}' is not available.");
        }
        if (sourcePrefab.IsReadOnly)
        {
            return CommandResult.Failure(
                "The source prefab is read-only.");
        }

        var write = assets.BeginApplyPrefabOverrides(
            sourcePrefab,
            EditorYaml.SerializePrefab(linkedPrefab));
        if (write is null)
        {
            return CommandResult.Failure(
                "Apply prefab could not prepare an atomic asset write.");
        }

        var atomicCancellationToken = CancellationToken.None;
        try
        {
            if (!await engine.RequestAssetReloadAsync(
                    atomicCancellationToken))
            {
                return await RollbackAsync(
                    assets,
                    write.Transaction,
                    engine,
                    "The native registry rejected the updated prefab.");
            }

            var commit = assets.CompletePrefabAssetWrite(
                write.Transaction,
                commit: true);
            if (!commit.Succeeded)
            {
                return await RollbackAsync(
                    assets,
                    write.Transaction,
                    engine,
                    AssetCommandMessages.Error(
                        commit,
                        "The prefab write transaction could not be committed"));
            }

            await engine.RefreshCurrentWorldAsync(
                atomicCancellationToken);
            return CommandResult.Success(
                "Prefab changes applied.",
                linkedPrefab.FileId);
        }
        catch (Exception exception)
        {
            return await RollbackAsync(
                assets,
                write.Transaction,
                engine,
                $"Apply prefab failed: {exception.Message}");
        }
    }

    static async Task<CommandResult> RollbackAsync(
        AssetsService assets,
        ProjectContentAssetWriteTransaction transaction,
        EngineService engine,
        string reason)
    {
        var diagnostics = new List<string> { reason };
        if (transaction.IsActive)
        {
            var rollback = assets.CompletePrefabAssetWrite(
                transaction,
                commit: false);
            if (!rollback.Succeeded)
            {
                diagnostics.Add(AssetCommandMessages.Error(
                    rollback,
                    "Prefab rollback failed"));
            }
        }

        try
        {
            if (!await engine.RequestAssetReloadAsync(
                    CancellationToken.None))
            {
                diagnostics.Add(
                    "The native registry rejected the rollback reload.");
            }
            await engine.RefreshCurrentWorldAsync(
                CancellationToken.None);
        }
        catch (Exception exception)
        {
            diagnostics.Add(
                $"Prefab rollback reconciliation failed: {exception.Message}");
        }

        return CommandResult.Failure(string.Join(" ", diagnostics));
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
