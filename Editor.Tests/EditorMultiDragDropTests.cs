using SailorEditor;
using SailorEditor.Commands;
using SailorEditor.Services;
using SailorEditor.Scene;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEngine;

namespace Editor.Tests;

[Collection("Editor drag/drop")]
public sealed class EditorMultiDragDropTests
{
    [Fact]
    public void DragSelectedItem_UsesSnapshotOfSelectionAndOtherItemRemainsSingleton()
    {
        var first = new ModelFile { FileId = new FileId("first") };
        var second = new PrefabFile { FileId = new FileId("second") };
        var items = new List<object> { first, second };
        var payload = Assert.IsType<EditorSelectionDragPayload>(EditorDragDrop.CreateSelectionPayload(items, second));
        items.Clear();
        Assert.Equal(new object[] { first, second }, payload.Items);
        var other = new MaterialFile();
        Assert.Same(other, EditorDragDrop.CreateSelectionPayload(payload.Items, other));
    }

    [Fact]
    public void SelectedAncestor_CoversDescendantsAcrossLinkedPrefabBoundary()
    {
        var parent = new GameObject { InstanceId = new InstanceId("parent") };
        var linkedRoot = new GameObject { InstanceId = new InstanceId("linked"), PrefabIndex = 1 };
        var world = MauiProgram.GetService<WorldService>();
        world.Current.Prefabs.Clear();
        world.Current.Prefabs.Add(new PrefabState());
        world.Current.Prefabs[0].GameObjects.Add(parent);
        world.Current.Prefabs.Add(new PrefabState { ParentInstanceId = "parent" });
        world.Current.Prefabs[1].GameObjects.Add(linkedRoot);

        Assert.Equal(new object[] { parent }, EditorDragDrop.GetSelectionRoots(new object[] { linkedRoot, parent }));
        Assert.Same(parent, EditorDragDrop.CreateSelectionPayload(new object[] { parent, linkedRoot }, linkedRoot));
    }

    [Fact]
    public void SceneGroup_RejectsEntireDropWhenDestinationIsOneSelectedObjectsDescendant()
    {
        var first = new GameObject { InstanceId = new InstanceId("first") };
        var second = new GameObject { InstanceId = new InstanceId("second") };
        var child = new GameObject { InstanceId = new InstanceId("child"), ParentIndex = 1 };
        SetWorld(first, second, child);
        var payload = new EditorSelectionDragPayload(new[] { first, second });

        Assert.False(EditorDragDrop.TryCreateSceneDropCommand(payload, child, out var command));
        Assert.Null(command);
    }

    [Fact]
    public void SceneGroup_ReparentsEveryIndependentRootAsOneUndoableCommand()
    {
        var first = new GameObject { InstanceId = new InstanceId("first") };
        var second = new GameObject { InstanceId = new InstanceId("second") };
        var target = new GameObject { InstanceId = new InstanceId("target") };
        SetWorld(first, second, target);

        Assert.True(EditorDragDrop.TryCreateSceneDropCommand(new EditorSelectionDragPayload(new[] { first, second }), target, out var command));
        Assert.IsType<CompoundEditorCommand>(command);
    }

    [Fact]
    public void SceneAssetDrop_RejectsRemovedTargetInsteadOfInstantiatingAtRoot()
    {
        SetWorld();
        var removedTarget = new GameObject { InstanceId = new InstanceId("removed") };
        var prefab = new PrefabFile { FileId = new FileId("prefab") };
        Assert.False(EditorDragDrop.TryCreateSceneDropCommand(prefab, removedTarget, out var command));
        Assert.Null(command);
    }

    [Fact]
    public void SelectedContentFolder_CoversNestedAssetsAndFolders()
    {
        var folder = new AssetFolder { FullPath = Path.GetFullPath("/content/Parent"), Id = 1 };
        var nested = new AssetFolder { FullPath = Path.GetFullPath("/content/Parent/Nested"), Id = 2 };
        var asset = new ModelFile { Asset = new FileInfo("/content/Parent/model.glb") };
        var sibling = new ModelFile { Asset = new FileInfo("/content/ParentOther/model.glb") };

        Assert.Equal(new object[] { folder, sibling }, EditorDragDrop.GetSelectionRoots(new object[] { folder, nested, asset, sibling }));
    }

    [Fact]
    public void FolderGroup_RejectsSelfAndDescendantDestinations()
    {
        var folder = new AssetFolder { FullPath = Path.GetFullPath("/content/Parent"), Id = 1 };
        var sibling = new AssetFolder { FullPath = Path.GetFullPath("/content/Other"), Id = 2 };
        var descendant = new AssetFolder { FullPath = Path.GetFullPath("/content/Parent/Nested"), Id = 3 };
        var payload = new EditorSelectionDragPayload(new[] { folder, sibling });

        Assert.False(EditorDragDrop.TryCreateContentDropCommand(payload, folder, out _, out _));
        Assert.False(EditorDragDrop.TryCreateContentDropCommand(payload, descendant, out _, out _));
    }

    [Fact]
    public void MixedPrefabAndModelGroup_InstantiatesTogetherInHierarchyAndViewport()
    {
        var payload = new EditorSelectionDragPayload(new AssetFile[]
        {
            new ModelFile { FileId = new FileId("model") },
            new PrefabFile { FileId = new FileId("prefab") }
        });

        Assert.True(EditorDragDrop.IsViewportAssetDrop(payload));
        Assert.True(EditorDragDrop.TryCreateSceneDropCommand(payload, null, out var sceneCommand));
        Assert.IsType<CompoundEditorCommand>(sceneCommand);
        Assert.True(EditorDragDrop.TryCreateViewportDropCommand(payload, new Vec4 { X = 1, Y = 2, Z = 3, W = 1 }, out var viewportCommand));
        Assert.IsType<CompoundEditorCommand>(viewportCommand);
    }

    [Fact]
    public void GroupPayload_CannotSilentlyAssignScalarAssetReferenceOrOverwriteOnePrefab()
    {
        var payload = new EditorSelectionDragPayload(new AssetFile[]
        {
            new ModelFile { FileId = new FileId("first") },
            new ModelFile { FileId = new FileId("second") }
        });

        Assert.False(EditorDragDrop.TryResolveAssetFileId(payload, null, _ => new ModelFile(), out var fileId));
        Assert.Null(fileId);
        Assert.False(EditorDragDrop.TryCreateContentDropCommand(payload, new PrefabFile(), out _, out var confirmation));
        Assert.False(confirmation);
    }

    [Fact]
    public void NativeViewportGroup_UsesOneOpaqueNativeUuidAndRetainsAllImmutableIds()
    {
        var first = new FileId("01234567-89AB-CDEF-0123-456789ABCDEF");
        var second = new FileId("11234567-89AB-CDEF-0123-456789ABCDEF");
        var expected = new[] { first.Value, second.Value };
        Assert.True(SceneViewportAssetDropPayload.TryCreate(new[] { first, second }, 7, out var payload));
        var token = payload[SceneViewportAssetDropPayload.Prefix.Length..];
        Assert.True(SceneViewportAssetDropPayload.IsSerializedFileId(token));
        Assert.True(payload.Length <= SceneViewportAssetDropPayload.MaxLength);
        Assert.DoesNotContain(first.Value, payload);

        first.Value = "changed after drag started";
        Assert.True(SceneViewportAssetDropPayload.TryResolveFileIds(token, 7, out var resolved));
        Assert.Equal(expected, resolved.Select(id => id.Value));
    }

    [Fact]
    public void NativeViewportGroup_RejectsOtherWorkspaceAndResetToken()
    {
        Assert.True(SceneViewportAssetDropPayload.TryCreate(new[]
        {
            new FileId("01234567-89AB-CDEF-0123-456789ABCDEF"),
            new FileId("11234567-89AB-CDEF-0123-456789ABCDEF")
        }, 8, out var payload));
        var token = payload[SceneViewportAssetDropPayload.Prefix.Length..];

        Assert.False(SceneViewportAssetDropPayload.TryResolveFileIds(token, 9, out var otherWorkspace));
        Assert.Empty(otherWorkspace);
        SceneViewportAssetDropPayload.ResetSelections();
        Assert.False(SceneViewportAssetDropPayload.TryResolveFileIds(token, 8, out var resetSelection));
        Assert.Empty(resetSelection);
        Assert.False(SceneViewportAssetDropPayload.TryResolveFileIds("{" + token + "}", 8, out _));
    }

    [Fact]
    public void NativeViewportGroup_RejectsEvictedTokenAndNeverUsesItsFirstAsset()
    {
        SceneViewportAssetDropPayload.ResetSelections();
        var ids = new[]
        {
            new FileId("01234567-89AB-CDEF-0123-456789ABCDEF"),
            new FileId("11234567-89AB-CDEF-0123-456789ABCDEF")
        };
        Assert.True(SceneViewportAssetDropPayload.TryCreate(ids, 1, out var first));
        var last = string.Empty;
        for (var index = 0; index < 32; index++)
        {
            Assert.True(SceneViewportAssetDropPayload.TryCreate(ids, 1, out last));
        }

        Assert.False(SceneViewportAssetDropPayload.TryResolveFileIds(first[SceneViewportAssetDropPayload.Prefix.Length..], 1, out _));
        Assert.True(SceneViewportAssetDropPayload.TryResolveFileIds(last[SceneViewportAssetDropPayload.Prefix.Length..], 1, out var resolved));
        Assert.Equal(2, resolved.Count);
    }

    [Fact]
    public void NativeViewportGroup_RejectsEntireMalformedGroupAndKeepsScalarDrop()
    {
        var valid = new FileId("01234567-89AB-CDEF-0123-456789ABCDEF");
        Assert.False(SceneViewportAssetDropPayload.TryCreate(new[] { valid, new FileId("invalid") }, 1, out var payload));
        Assert.Empty(payload);
        Assert.True(SceneViewportAssetDropPayload.TryResolveFileIds(valid.Value, 1, out var scalar));
        Assert.Equal(valid, Assert.Single(scalar));
    }

    static void SetWorld(params GameObject[] gameObjects)
    {
        var world = MauiProgram.GetService<WorldService>();
        world.Current.Prefabs.Clear();
        world.Current.Prefabs.Add(new PrefabState());
        world.Current.Prefabs[0].GameObjects.AddRange(gameObjects);
    }
}
