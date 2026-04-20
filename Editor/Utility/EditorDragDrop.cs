using SailorEditor.Commands;
using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEngine;
using System;

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

        return false;
    }

    public static bool TryCreateContentDropCommand(object? source, object? target, out CreatePrefabAssetCommand? command, out bool requiresConfirmation)
    {
        requiresConfirmation = false;
        var currentSource = source is GameObject gameObject ? ResolveCurrentGameObject(gameObject) : null;

        command = source switch
        {
            GameObject when currentSource is not null && target is null => new CreatePrefabAssetCommand(currentSource),
            GameObject when currentSource is not null && target is AssetFolder folder => new CreatePrefabAssetCommand(currentSource, folder),
            GameObject when currentSource is not null && target is PrefabFile prefab => new CreatePrefabAssetCommand(currentSource, existingPrefab: prefab),
            _ => null
        };

        if (command is null)
            return false;

        requiresConfirmation = target is PrefabFile;
        return true;
    }

    static GameObject? ResolveCurrentGameObject(GameObject gameObject)
    {
        if (gameObject.InstanceId is null || gameObject.InstanceId.IsEmpty())
            return gameObject;

        var world = MauiProgram.GetService<WorldService>();
        return world.TryGetGameObject(gameObject.InstanceId, out var current) ? current : null;
    }

    static bool TryResolvePrefabAsset(object? source, out AssetFile prefab)
    {
        prefab = null;
        if (source is not AssetFile assetFile || assetFile.FileId is null || assetFile.FileId.IsEmpty())
            return false;

        if (assetFile is PrefabFile ||
            string.Equals(assetFile.AssetInfoTypeName, "Sailor::PrefabAssetInfo", StringComparison.Ordinal) ||
            string.Equals(assetFile.Asset?.Extension, ".prefab", StringComparison.OrdinalIgnoreCase))
        {
            prefab = assetFile;
            return true;
        }

        return false;
    }

    static bool CanReparent(GameObject source, GameObject? target)
    {
        if (target is null)
            return source.ParentIndex != uint.MaxValue;

        if (ReferenceEquals(source, target) || HasSameInstanceId(source, target))
            return false;

        if (HasSameParent(source, target))
            return false;

        return !IsDescendantOf(target, source);
    }

    static bool HasSameParent(GameObject source, GameObject target)
    {
        if (source.ParentIndex == uint.MaxValue)
            return false;

        if (source.PrefabIndex != target.PrefabIndex)
            return false;

        var prefab = MauiProgram.GetService<Services.WorldService>().Current.Prefabs[source.PrefabIndex];
        if ((int)source.ParentIndex >= prefab.GameObjects.Count)
            return false;

        return ReferenceEquals(prefab.GameObjects[(int)source.ParentIndex], target) || HasSameInstanceId(prefab.GameObjects[(int)source.ParentIndex], target);
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
