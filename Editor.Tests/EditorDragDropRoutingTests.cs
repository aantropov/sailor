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

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void TryResolveAssetFileId_EnforcesAnimatorAssetTypes(bool controllerField)
    {
        var expectedFileId = new FileId(controllerField ? "{CONTROLLER}" : "{ANIMSET}");
        var wrongFileId = new FileId("{MATERIAL}");
        AssetFile expectedAsset = controllerField
            ? new AnimationControllerFile { FileId = expectedFileId }
            : new AnimationSetFile { FileId = expectedFileId };
        var wrongAsset = new MaterialFile { FileId = wrongFileId };
        var supportedType = controllerField
            ? typeof(AnimationControllerFile)
            : typeof(AnimationSetFile);

        Assert.True(EditorDragDrop.TryResolveAssetFileId(
            expectedAsset,
            supportedType,
            id => id == expectedFileId ? expectedAsset : null,
            out var resolvedExpected));
        Assert.Equal(expectedFileId, resolvedExpected);

        Assert.False(EditorDragDrop.TryResolveAssetFileId(
            wrongAsset,
            supportedType,
            id => id == wrongFileId ? wrongAsset : null,
            out _));
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
        Assert.IsAssignableFrom<IUndoableEditorCommand>(command);
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
    public void TryCreateSceneDropCommand_RoutesModelCreationToTarget()
    {
        var model = new ModelFile
        {
            FileId = new FileId("{MODEL}"),
            Asset = new FileInfo("/Content/Models/Duck.glb")
        };
        var target = new GameObject
        {
            InstanceId = new InstanceId("go-target")
        };

        var resolved = EditorDragDrop.TryCreateSceneDropCommand(
            model,
            target,
            out var command);

        Assert.True(resolved);
        Assert.IsType<CreateModelGameObjectCommand>(command);
        Assert.IsAssignableFrom<IUndoableEditorCommand>(command);
        Assert.Equal("Duck", EditorDragDrop.ResolveAssetObjectName(model));
    }

    [Fact]
    public void TryCreateViewportDropCommand_RoutesModelAndPrefabAtResolvedPosition()
    {
        var position = new Vec4 { X = 1, Y = 2, Z = 3, W = 1 };
        var model = new ModelFile
        {
            FileId = new FileId("{MODEL}"),
            Asset = new FileInfo("/Content/Models/Duck.glb")
        };
        var prefab = new PrefabFile
        {
            FileId = new FileId("{PREFAB}")
        };

        Assert.True(EditorDragDrop.TryCreateViewportDropCommand(
            model,
            position,
            out var modelCommand));
        Assert.IsType<CreateModelGameObjectCommand>(modelCommand);
        Assert.IsAssignableFrom<IUndoableEditorCommand>(modelCommand);
        Assert.True(EditorDragDrop.TryCreateViewportDropCommand(
            prefab,
            position,
            out var prefabCommand));
        Assert.IsType<InstantiatePrefabAssetCommand>(prefabCommand);
        Assert.IsAssignableFrom<IUndoableEditorCommand>(prefabCommand);
    }

    [Fact]
    public void IsViewportAssetDrop_RecognizesAssetInfoTypeFallbacks()
    {
        var model = new AssetFile
        {
            FileId = new FileId("{MODEL}"),
            AssetInfoTypeName = "Sailor::ModelAssetInfo"
        };
        var prefab = new AssetFile
        {
            FileId = new FileId("{PREFAB}"),
            AssetInfoTypeName = "Sailor::PrefabAssetInfo"
        };

        Assert.True(EditorDragDrop.IsViewportAssetDrop(model));
        Assert.True(EditorDragDrop.IsViewportAssetDrop(prefab));
        Assert.False(EditorDragDrop.IsViewportAssetDrop(
            new MaterialFile { FileId = new FileId("{MATERIAL}") }));
    }

    [Fact]
    public void IsViewportAssetDrop_UsesExplicitAssetInfoTypeBeforeSourceExtension()
    {
        var secondaryTextureFromGlb = new AssetFile
        {
            FileId = new FileId("{TEXTURE}"),
            AssetInfoTypeName = "Sailor::TextureAssetInfo",
            Asset = new FileInfo("/Content/Models/Duck.glb")
        };
        var nonPrefabWithPrefabFilename = new AssetFile
        {
            FileId = new FileId("{MATERIAL}"),
            AssetInfoTypeName = "Sailor::MaterialAssetInfo",
            Asset = new FileInfo("/Content/Prefabs/Duck.prefab")
        };
        var legacyModel = new AssetFile
        {
            FileId = new FileId("{LEGACY-MODEL}"),
            Asset = new FileInfo("/Content/Models/Legacy.glb")
        };
        var genericModel = new AssetFile
        {
            FileId = new FileId("{GENERIC-MODEL}"),
            AssetInfoTypeName = "Sailor::AssetInfo",
            Asset = new FileInfo("/Content/Models/Generic.obj")
        };

        Assert.False(EditorDragDrop.IsViewportAssetDrop(
            secondaryTextureFromGlb));
        Assert.False(EditorDragDrop.IsViewportAssetDrop(
            nonPrefabWithPrefabFilename));
        Assert.True(EditorDragDrop.IsViewportAssetDrop(legacyModel));
        Assert.True(EditorDragDrop.IsViewportAssetDrop(genericModel));
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
    public void TryCreateSceneDropCommand_RoutesExternallyParentedRootToWorldRoot()
    {
        var parent = new GameObject
        {
            InstanceId = new InstanceId("go-parent"),
            PrefabIndex = 0,
            ParentIndex = uint.MaxValue
        };
        var child = new GameObject
        {
            InstanceId = new InstanceId("go-external-child"),
            PrefabIndex = 1,
            ParentIndex = uint.MaxValue
        };
        var worldService =
            SailorEditor.MauiProgram
                .GetService<SailorEditor.Services.WorldService>();
        worldService.Current.Prefabs.Clear();
        var parentPrefab = new SailorEditor.Services.PrefabState();
        parentPrefab.GameObjects.Add(parent);
        var childPrefab = new SailorEditor.Services.PrefabState
        {
            ParentInstanceId = parent.InstanceId!.Value
        };
        childPrefab.GameObjects.Add(child);
        worldService.Current.Prefabs.Add(parentPrefab);
        worldService.Current.Prefabs.Add(childPrefab);

        var resolved = EditorDragDrop.TryCreateSceneDropCommand(
            child,
            null,
            out var command);

        Assert.True(resolved);
        Assert.IsType<ReparentGameObjectCommand>(command);
    }

    [Fact]
    public void TryCreateSceneDropCommand_RejectsNoOpReparentToExternalParent()
    {
        var parent = new GameObject
        {
            InstanceId = new InstanceId("go-parent"),
            PrefabIndex = 0,
            ParentIndex = uint.MaxValue
        };
        var child = new GameObject
        {
            InstanceId = new InstanceId("go-external-child"),
            PrefabIndex = 1,
            ParentIndex = uint.MaxValue
        };
        var worldService =
            SailorEditor.MauiProgram
                .GetService<SailorEditor.Services.WorldService>();
        worldService.Current.Prefabs.Clear();
        var parentPrefab = new SailorEditor.Services.PrefabState();
        parentPrefab.GameObjects.Add(parent);
        var childPrefab = new SailorEditor.Services.PrefabState
        {
            ParentInstanceId = parent.InstanceId!.Value
        };
        childPrefab.GameObjects.Add(child);
        worldService.Current.Prefabs.Add(parentPrefab);
        worldService.Current.Prefabs.Add(childPrefab);

        var resolved = EditorDragDrop.TryCreateSceneDropCommand(
            child,
            parent,
            out var command);

        Assert.False(resolved);
        Assert.Null(command);
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
