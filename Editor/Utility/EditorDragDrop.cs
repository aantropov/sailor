using SailorEditor.Commands;
using SailorEditor.ViewModels;
using SailorEngine;

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
        command = source switch
        {
            GameObject gameObject when target is null || (!ReferenceEquals(gameObject, target) && !IsDescendantOf(target, gameObject))
                => new ReparentGameObjectCommand(gameObject, target, keepWorldTransform: true),
            PrefabFile prefab when target is null || target.InstanceId is not null
                => new InstantiatePrefabAssetCommand(prefab, target),
            _ => null
        };

        return command is not null;
    }

    public static bool TryCreateContentDropCommand(object? source, object? target, out CreatePrefabAssetCommand? command, out bool requiresConfirmation)
    {
        requiresConfirmation = false;
        command = source switch
        {
            GameObject gameObject when target is null => new CreatePrefabAssetCommand(gameObject),
            GameObject gameObject when target is AssetFolder folder => new CreatePrefabAssetCommand(gameObject, folder),
            GameObject gameObject when target is PrefabFile prefab => new CreatePrefabAssetCommand(gameObject, existingPrefab: prefab),
            _ => null
        };

        if (command is null)
            return false;

        requiresConfirmation = target is PrefabFile;
        return true;
    }

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
            if (ReferenceEquals(current, parent))
                return true;
        }

        return false;
    }
}

public sealed class TreeViewDropRequest
{
    public object? Source { get; init; }
    public object? Target { get; init; }
    public bool Handled { get; set; }
}
