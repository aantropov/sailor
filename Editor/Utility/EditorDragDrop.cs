using SailorEditor.Commands;
using SailorEditor.Content;
using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEngine;
using System;
using System.Diagnostics.CodeAnalysis;

namespace SailorEditor.Utility;

public static class EditorDragDrop
{
    public const string DragItemKey = "DragItem";

    public static bool TryResolveAssetFileId(object? droppedItem, Type? supportedType, Func<FileId, AssetFile?> assetLookup, out FileId? fileId)
    {
        fileId = droppedItem switch
        {
            FileId id when !id.IsEmpty() => id,
            AssetFile asset when asset.FileId is not null && !asset.FileId.IsEmpty() => asset.FileId,
            _ => null
        };

        if (fileId is null)
            return false;

        var resolvedAsset = assetLookup(fileId);
        if (resolvedAsset is null)
            return false;

        if (supportedType is not null && !supportedType.IsInstanceOfType(resolvedAsset))
            return false;

        return true;
    }

    public static bool TryCreateSceneDropCommand(object? source, GameObject? target, out IEditorCommand? command)
    {
        var currentSource = source is GameObject sourceGameObject ? ResolveCurrentGameObject(sourceGameObject) : null;
        var currentTarget = target is not null ? ResolveCurrentGameObject(target) : null;

        command = null;

        if (source is GameObject && currentSource is not null && CanReparent(currentSource, currentTarget))
        {
            command = new ReparentGameObjectCommand(currentSource, currentTarget, keepWorldTransform: true);
            return true;
        }

        if (TryResolvePrefabAsset(source, out var prefab) && (currentTarget is null || currentTarget.InstanceId is not null))
        {
            command = new InstantiatePrefabAssetCommand(prefab, currentTarget);
            return true;
        }

        if (TryResolveModelAsset(source, out var model) &&
            (currentTarget is null || currentTarget.InstanceId is not null))
        {
            command = new CreateModelGameObjectCommand(
                model,
                ResolveAssetObjectName(model),
                currentTarget);
            return true;
        }

        return false;
    }

    public static bool TryCreateViewportDropCommand(
        object? source,
        Vec4 worldPosition,
        out IEditorCommand? command)
    {
        command = null;
        if (worldPosition is null)
        {
            return false;
        }

        if (TryResolvePrefabAsset(source, out var prefab))
        {
            command = new InstantiatePrefabAssetCommand(
                prefab,
                worldPosition: worldPosition);
            return true;
        }

        if (TryResolveModelAsset(source, out var model))
        {
            command = new CreateModelGameObjectCommand(
                model,
                ResolveAssetObjectName(model),
                worldPosition: worldPosition);
            return true;
        }

        return false;
    }

    public static bool IsViewportAssetDrop(object? source) =>
        TryResolvePrefabAsset(source, out _) ||
        TryResolveModelAsset(source, out _);

    public static bool TryCreateContentDropCommand(object? source, object? target, out IEditorCommand? command, out bool requiresConfirmation)
    {
        requiresConfirmation = false;
        var currentSource = source is GameObject gameObject ? ResolveCurrentGameObject(gameObject) : null;

        command = source switch
        {
            GameObject when currentSource is not null && target is null => new CreatePrefabAssetCommand(currentSource),
            GameObject when currentSource is not null && target is AssetFolder { IsReadOnly: false } folder => new CreatePrefabAssetCommand(currentSource, folder),
            GameObject when currentSource is not null && target is PrefabFile { IsReadOnly: false } prefab => new CreatePrefabAssetCommand(currentSource, existingPrefab: prefab),
            AssetFile asset when !asset.IsReadOnly && target is null => new MoveAssetCommand(asset),
            AssetFile asset when !asset.IsReadOnly && target is AssetFolder { IsReadOnly: false } targetFolder => new MoveAssetCommand(asset, targetFolder),
            AssetFolder sourceFolder when IsMovableFolder(sourceFolder) && target is null => new MoveFolderCommand(sourceFolder),
            AssetFolder sourceFolder when IsMovableFolder(sourceFolder) && target is AssetFolder { IsReadOnly: false } targetFolder => new MoveFolderCommand(sourceFolder, targetFolder),
            _ => null
        };

        if (command is null)
            return false;

        requiresConfirmation = target is PrefabFile;
        return true;
    }

    static bool IsMovableFolder(AssetFolder folder)
        => !folder.IsReadOnly
            && folder.Id is not ProjectContentFolderIds.ContentRootId
            && folder.Id is not ProjectContentFolderIds.EngineContentRootId;

    static GameObject? ResolveCurrentGameObject(GameObject gameObject)
    {
        if (gameObject.InstanceId is null || gameObject.InstanceId.IsEmpty())
            return gameObject;

        var world = MauiProgram.GetService<WorldService>();
        return world.TryGetGameObject(gameObject.InstanceId, out var current) ? current : null;
    }

    static bool TryResolvePrefabAsset(object? source, [NotNullWhen(true)] out AssetFile? prefab)
    {
        prefab = null;
        if (source is not AssetFile assetFile || assetFile.FileId is null || assetFile.FileId.IsEmpty())
            return false;

        if (assetFile is PrefabFile)
        {
            prefab = assetFile;
            return true;
        }

        if (HasAuthoritativeAssetInfoType(assetFile))
        {
            if (string.Equals(
                    assetFile.AssetInfoTypeName,
                    "Sailor::PrefabAssetInfo",
                    StringComparison.Ordinal))
            {
                prefab = assetFile;
                return true;
            }

            return false;
        }

        if (string.Equals(
                assetFile.Asset?.Extension,
                ".prefab",
                StringComparison.OrdinalIgnoreCase))
        {
            prefab = assetFile;
            return true;
        }

        return false;
    }

    static bool TryResolveModelAsset(object? source, [NotNullWhen(true)] out AssetFile? model)
    {
        model = null;
        if (source is not AssetFile assetFile ||
            assetFile.FileId is null ||
            assetFile.FileId.IsEmpty())
        {
            return false;
        }

        if (assetFile is ModelFile)
        {
            model = assetFile;
            return true;
        }

        if (HasAuthoritativeAssetInfoType(assetFile))
        {
            if (string.Equals(
                    assetFile.AssetInfoTypeName,
                    "Sailor::ModelAssetInfo",
                    StringComparison.Ordinal))
            {
                model = assetFile;
                return true;
            }

            return false;
        }

        var extension = assetFile.Asset?.Extension;
        if (string.Equals(extension, ".obj", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(extension, ".gltf", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(extension, ".glb", StringComparison.OrdinalIgnoreCase))
        {
            model = assetFile;
            return true;
        }

        return false;
    }

    static bool HasAuthoritativeAssetInfoType(AssetFile assetFile)
        => !string.IsNullOrWhiteSpace(assetFile.AssetInfoTypeName) &&
            !string.Equals(
                assetFile.AssetInfoTypeName,
                AssetFile.DefaultAssetInfoTypeName,
                StringComparison.Ordinal);

    public static string ResolveAssetObjectName(AssetFile assetFile)
    {
        var filename = assetFile.Asset?.Name;
        if (string.IsNullOrWhiteSpace(filename))
        {
            return "GameObject";
        }

        var name = Path.GetFileNameWithoutExtension(filename);
        return string.IsNullOrWhiteSpace(name)
            ? "GameObject"
            : name;
    }

    static bool CanReparent(GameObject source, GameObject? target)
    {
        if (target is null)
        {
            return MauiProgram.GetService<WorldService>()
                .ResolveParentInstanceId(source) is not null;
        }

        if (ReferenceEquals(source, target) || HasSameInstanceId(source, target))
            return false;

        if (HasSameParent(source, target))
            return false;

        return !IsDescendantOf(target, source);
    }

    static bool HasSameParent(GameObject source, GameObject target)
    {
        var parentInstanceId = MauiProgram.GetService<WorldService>()
            .ResolveParentInstanceId(source);
        return parentInstanceId is not null
            && target.InstanceId is not null
            && string.Equals(
                parentInstanceId.Value,
                target.InstanceId.Value,
                StringComparison.Ordinal);
    }

    static bool HasSameInstanceId(GameObject? left, GameObject? right)
        => left?.InstanceId is not null
            && right?.InstanceId is not null
            && string.Equals(left.InstanceId.Value, right.InstanceId.Value, StringComparison.Ordinal);

    static bool IsDescendantOf(GameObject? candidate, GameObject parent)
    {
        if (candidate is null)
            return false;

        if (candidate.PrefabIndex != parent.PrefabIndex)
            return false;

        var prefab = MauiProgram.GetService<Services.WorldService>().Current.Prefabs[candidate.PrefabIndex];
        var current = candidate;
        while (current.ParentIndex != uint.MaxValue)
        {
            current = prefab.GameObjects[(int)current.ParentIndex];
            if (ReferenceEquals(current, parent) || HasSameInstanceId(current, parent))
                return true;
        }

        return false;
    }
}
