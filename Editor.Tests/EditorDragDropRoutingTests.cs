using SailorEditor.Commands;
using SailorEditor.Content;
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
    public void TryCreateContentDropCommand_RoutesWritableAssetMoveToFolder()
    {
        var source = new MaterialFile { FileId = new FileId("{MAT}") };
        var target = new AssetFolder { Id = 42 };

        var resolved = EditorDragDrop.TryCreateContentDropCommand(source, target, out var command, out var requiresConfirmation);

        Assert.True(resolved);
        Assert.False(requiresConfirmation);
        Assert.IsType<MoveAssetCommand>(command);
    }

    [Fact]
    public void TryCreateContentDropCommand_RoutesWritableAssetMoveToActiveRoot()
    {
        var source = new MaterialFile { FileId = new FileId("{MAT}") };

        var resolved = EditorDragDrop.TryCreateContentDropCommand(source, null, out var command, out var requiresConfirmation);

        Assert.True(resolved);
        Assert.False(requiresConfirmation);
        Assert.IsType<MoveAssetCommand>(command);
    }

    [Fact]
    public void TryCreateContentDropCommand_RoutesWritableFolderMove()
    {
        var source = new AssetFolder { Id = 41 };
        var target = new AssetFolder { Id = 42 };

        var resolved = EditorDragDrop.TryCreateContentDropCommand(source, target, out var command, out var requiresConfirmation);

        Assert.True(resolved);
        Assert.False(requiresConfirmation);
        Assert.IsType<MoveFolderCommand>(command);
    }

    [Theory]
    [InlineData(ProjectContentFolderIds.ContentRootId)]
    [InlineData(ProjectContentFolderIds.EngineContentRootId)]
    public void TryCreateContentDropCommand_AllowsWritableContentRootsAsMoveTargets(int rootId)
    {
        var source = new MaterialFile { FileId = new FileId("{MAT}") };
        var target = new AssetFolder { Id = rootId };

        var resolved = EditorDragDrop.TryCreateContentDropCommand(source, target, out var command, out _);

        Assert.True(resolved);
        Assert.IsType<MoveAssetCommand>(command);
    }

    [Fact]
    public void TryCreateContentDropCommand_RejectsAssetRowsAsMoveTargets()
    {
        var source = new MaterialFile { FileId = new FileId("{SOURCE}") };
        var target = new MaterialFile { FileId = new FileId("{TARGET}") };

        var resolved = EditorDragDrop.TryCreateContentDropCommand(source, target, out var command, out _);

        Assert.False(resolved);
        Assert.Null(command);
    }

    [Fact]
    public void TryCreateContentDropCommand_RejectsReadOnlyContent()
    {
        var readOnlyAsset = new MaterialFile { FileId = new FileId("{MAT}"), IsReadOnly = true };
        var readOnlyFolder = new AssetFolder { Id = 42, IsReadOnly = true };

        Assert.False(EditorDragDrop.TryCreateContentDropCommand(readOnlyAsset, null, out var assetCommand, out _));
        Assert.Null(assetCommand);
        Assert.False(EditorDragDrop.TryCreateContentDropCommand(new MaterialFile(), readOnlyFolder, out var targetCommand, out _));
        Assert.Null(targetCommand);
    }

    [Theory]
    [InlineData(ProjectContentFolderIds.ContentRootId)]
    [InlineData(ProjectContentFolderIds.EngineContentRootId)]
    public void TryCreateContentDropCommand_RejectsContentRootsAsMoveSources(int rootId)
    {
        var source = new AssetFolder { Id = rootId };

        var resolved = EditorDragDrop.TryCreateContentDropCommand(source, null, out var command, out _);

        Assert.False(resolved);
        Assert.Null(command);
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
    public void TryCreateSceneDropCommand_AllowsReadOnlyEnginePrefabAsReferenceSource()
    {
        var prefab = new PrefabFile
        {
            FileId = new FileId("{ENGINE-PREFAB}"),
            IsReadOnly = true
        };

        var resolved = EditorDragDrop.TryCreateSceneDropCommand(prefab, null, out var command);

        Assert.True(resolved);
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
    public void TryCreateSceneDropCommand_RoutesReparentToRoot_WhenSourceCurrentlyHasParent()
    {
        var parent = new GameObject { InstanceId = new InstanceId("go-parent"), PrefabIndex = 0, ParentIndex = uint.MaxValue };
        var child = new GameObject { InstanceId = new InstanceId("go-child"), PrefabIndex = 0, ParentIndex = 0 };
        var worldService = SailorEditor.MauiProgram.GetService<SailorEditor.Services.WorldService>();
        worldService.Current.Prefabs.Clear();
        worldService.Current.Prefabs.Add(new SailorEditor.Services.PrefabState());
        worldService.Current.Prefabs[0].GameObjects.Add(parent);
        worldService.Current.Prefabs[0].GameObjects.Add(child);

        var resolved = EditorDragDrop.TryCreateSceneDropCommand(child, null, out var command);

        Assert.True(resolved);
        Assert.NotNull(command);
        Assert.IsType<ReparentGameObjectCommand>(command);
    }

    [Fact]
    public void TryCreateSceneDropCommand_RejectsReparentIntoOwnDescendant()
    {
        var root = new GameObject { InstanceId = new InstanceId("go-root"), PrefabIndex = 0, ParentIndex = uint.MaxValue };
        var child = new GameObject { InstanceId = new InstanceId("go-child"), PrefabIndex = 0, ParentIndex = 0 };
        var grandChild = new GameObject { InstanceId = new InstanceId("go-grandchild"), PrefabIndex = 0, ParentIndex = 1 };
        var worldService = SailorEditor.MauiProgram.GetService<SailorEditor.Services.WorldService>();
        worldService.Current.Prefabs.Clear();
        worldService.Current.Prefabs.Add(new SailorEditor.Services.PrefabState());
        worldService.Current.Prefabs[0].GameObjects.Add(root);
        worldService.Current.Prefabs[0].GameObjects.Add(child);
        worldService.Current.Prefabs[0].GameObjects.Add(grandChild);

        var resolved = EditorDragDrop.TryCreateSceneDropCommand(root, grandChild, out var command);

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
