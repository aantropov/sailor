using SailorEditor.Commands;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEngine;

namespace Editor.Tests;

public sealed class EditorDragDropRoutingTests
{
    [Fact]
    public void TryResolveAssetFileId_AcceptsMatchingAssetFile()
    {
        var fileId = new FileId("{MAT}");
        var asset = new MaterialFile { FileId = fileId };

        var resolved = EditorDragDrop.TryResolveAssetFileId(
            asset,
            typeof(MaterialFile),
            id => id == fileId ? asset : null,
            out var result);

        Assert.True(resolved);
        Assert.Equal(fileId, result);
    }

    [Fact]
    public void TryResolveAssetFileId_RejectsMismatchedAssetType()
    {
        var fileId = new FileId("{MAT}");
        var asset = new MaterialFile { FileId = fileId };

        var resolved = EditorDragDrop.TryResolveAssetFileId(
            asset,
            typeof(TextureFile),
            id => id == fileId ? asset : null,
            out var result);

        Assert.False(resolved);
        Assert.Equal(fileId, result);
    }

    [Fact]
    public void TryCreateContentDropCommand_RoutesPrefabOverwriteThroughSingleCommand()
    {
        var source = new GameObject();
        var target = new PrefabFile { FileId = new FileId("{PREFAB}") };

        var resolved = EditorDragDrop.TryCreateContentDropCommand(source, target, out var command, out var requiresConfirmation);

        Assert.True(resolved);
        Assert.True(requiresConfirmation);
        Assert.NotNull(command);
        Assert.IsType<CreatePrefabAssetCommand>(command);
    }

    [Fact]
    public void TryCreateSceneDropCommand_RoutesPrefabInstantiationToTarget()
    {
        var prefab = new PrefabFile { FileId = new FileId("{PREFAB}") };
        var target = new GameObject { InstanceId = new InstanceId("go-target") };

        var resolved = EditorDragDrop.TryCreateSceneDropCommand(prefab, target, out var command);

        Assert.True(resolved);
        Assert.NotNull(command);
        Assert.IsType<InstantiatePrefabAssetCommand>(command);
    }

    [Fact]
    public void TryCreateSceneDropCommand_RejectsSelfReparent()
    {
        var gameObject = new GameObject { InstanceId = new InstanceId("go-self") };

        var resolved = EditorDragDrop.TryCreateSceneDropCommand(gameObject, gameObject, out var command);

        Assert.False(resolved);
        Assert.Null(command);
    }

    [Fact]
    public void TryCreateSceneDropCommand_RejectsNoOpReparentToSameParent()
    {
        var parent = new GameObject { InstanceId = new InstanceId("go-parent"), PrefabIndex = 0 };
        var child = new GameObject { InstanceId = new InstanceId("go-child"), PrefabIndex = 0, ParentIndex = 0 };
        var worldService = SailorEditor.MauiProgram.GetService<SailorEditor.Services.WorldService>();
        worldService.Current.Prefabs.Clear();
        worldService.Current.Prefabs.Add(new SailorEditor.Services.PrefabState());
        worldService.Current.Prefabs[0].GameObjects.Add(parent);
        worldService.Current.Prefabs[0].GameObjects.Add(child);

        var resolved = EditorDragDrop.TryCreateSceneDropCommand(child, parent, out var command);

        Assert.False(resolved);
        Assert.Null(command);
    }

    [Fact]
    public void TryCreateSceneDropCommand_RoutesReparentToRootOnlyWhenParentActuallyChanges()
    {
        var root = new GameObject { InstanceId = new InstanceId("go-root"), PrefabIndex = 0, ParentIndex = uint.MaxValue };

        var resolved = EditorDragDrop.TryCreateSceneDropCommand(root, null, out var command);

        Assert.False(resolved);
        Assert.Null(command);
    }
}
