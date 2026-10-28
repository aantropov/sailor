using SailorEditor;
using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEngine;

namespace Editor.Tests.EngineIntegration;

[Collection("LocalEngineProtocolTransport")]
public sealed class InspectorSelectionTests : IDisposable
{
    readonly SelectionService selection = new();
    readonly WorldService world = new();
    readonly InspectorProjectionService inspector;

    public InspectorSelectionTests()
    {
        MauiProgram.SetService(world);
        inspector = new InspectorProjectionService(selection, world);
    }

    [Fact]
    public async Task EmptyScenePick_RetainsMaterialInspector_WhileClearingActualSelection()
    {
        var material = new MaterialFile { FileId = new("material-preview-fixture"), IsMetadataLoaded = true };
        await selection.SelectObjectAsync(material);
        selection.ApplyRuntimeSelection(null);

        Assert.Null(selection.SelectedItem);
        Assert.Empty(selection.SelectedItems);
        Assert.True(selection.SelectedInstanceId.IsEmpty());
        Assert.Same(material, selection.InspectedItem);
        Assert.Same(material, inspector.SelectedItem);

        selection.ClearSelection();
        Assert.Null(selection.InspectedItem);
        Assert.Null(inspector.SelectedItem);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void EmptyScenePick_RetainsSceneInspector_UntilObjectIsDeleted(bool component)
    {
        var gameObject = new GameObject { InstanceId = new("object-1") };
        var child = new Component { InstanceId = new("component-1"), Owner = gameObject };
        world.GameObjects.Add(gameObject.InstanceId.Value, gameObject);
        world.Components.Add(child.InstanceId.Value, child);
        var id = component ? child.InstanceId : gameObject.InstanceId;
        selection.SelectInstance(id);
        var inspected = inspector.SelectedItem;
        selection.ApplyRuntimeSelection(InstanceId.NullInstanceId);
        Assert.Same(inspected, inspector.SelectedItem);
        Assert.True(selection.SelectedInstanceId.IsEmpty());

        world.GameObjects.Clear();
        world.Components.Clear();
        world.PublishUpdate();
        Assert.Null(selection.InspectedItem);
        Assert.Null(inspector.SelectedItem);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task DocumentOrWorkspaceReset_ClearsRetainedInspector(bool workspace)
    {
        var material = new MaterialFile { FileId = new("retained-material"), IsMetadataLoaded = true };
        await selection.SelectObjectAsync(material);
        selection.ApplyRuntimeSelection(null);
        var resetGeneration = inspector.ResetGeneration;
        if (workspace)
        {
            selection.ResetForWorkspaceChange();
        }
        else
        {
            selection.ResetForDocumentChange();
        }

        Assert.Null(selection.InspectedItem);
        Assert.Null(inspector.SelectedItem);
        Assert.True(inspector.ResetGeneration > resetGeneration);
    }

    [Fact]
    public async Task AssetRefresh_RebindsRetainedInspector_WithoutSelectingIt_AndDeletionClearsIt()
    {
        var original = new MaterialFile { FileId = new("retained-material"), IsMetadataLoaded = true };
        await selection.SelectObjectAsync(original);
        selection.ApplyRuntimeSelection(null);
        var replacement = new MaterialFile { FileId = original.FileId, IsMetadataLoaded = true };
        await selection.RebindInspectedAssetAsync(original, replacement);

        Assert.Same(replacement, inspector.SelectedItem);
        Assert.Null(selection.SelectedItem);
        Assert.Empty(selection.SelectedItems);
        await selection.RebindInspectedAssetAsync(replacement, null);
        Assert.Null(inspector.SelectedItem);
    }

    [Fact]
    public async Task DelayedAssetRefresh_DoesNotReplaceNewInspection()
    {
        var original = new MaterialFile { FileId = new("retained-material"), IsMetadataLoaded = true };
        await selection.SelectObjectAsync(original);
        selection.ApplyRuntimeSelection(null);
        var replacement = new DelayedAsset { FileId = original.FileId, IsMetadataLoaded = true };
        var refresh = selection.RebindInspectedAssetAsync(original, replacement);
        Assert.False(refresh.IsCompleted);
        var next = new MaterialFile { FileId = new("new-material"), IsMetadataLoaded = true };
        await selection.SelectObjectAsync(next);
        replacement.Ready.SetResult();
        await refresh;
        Assert.Same(next, inspector.SelectedItem);
    }

    [Fact]
    public async Task EmptyScenePick_DuringAssetRefresh_DoesNotDiscardInspectionReplacement()
    {
        var original = new MaterialFile { FileId = new("retained-material"), IsMetadataLoaded = true };
        await selection.SelectObjectAsync(original);
        var replacement = new DelayedAsset { FileId = original.FileId, IsMetadataLoaded = true };
        var refresh = selection.RebindInspectedAssetAsync(original, replacement);
        selection.ApplyRuntimeSelection(null);
        Assert.Same(original, inspector.SelectedItem);
        replacement.Ready.SetResult();
        await refresh;
        Assert.Same(replacement, inspector.SelectedItem);
        Assert.Null(selection.SelectedItem);
    }

    [Fact]
    public async Task OverlappingAssetRefreshes_PublishOnlyNewestRequest()
    {
        var original = new MaterialFile { FileId = new("retained-material"), IsMetadataLoaded = true };
        await selection.SelectObjectAsync(original);
        selection.ApplyRuntimeSelection(null);
        var older = new DelayedAsset { FileId = original.FileId, IsMetadataLoaded = true };
        var newest = new DelayedAsset { FileId = original.FileId, IsMetadataLoaded = true };
        var olderRefresh = selection.RebindInspectedAssetAsync(original, older);
        var newestRefresh = selection.RebindInspectedAssetAsync(original, newest);

        older.Ready.SetResult();
        await olderRefresh;
        Assert.Same(original, inspector.SelectedItem);
        newest.Ready.SetResult();
        await newestRefresh;
        Assert.Same(newest, inspector.SelectedItem);
        Assert.Null(selection.SelectedItem);
    }

    [Fact]
    public void WorldRefresh_RebindsRetainedObject_WithoutRestoringSceneSelection()
    {
        var original = new GameObject { InstanceId = new("object-1") };
        world.GameObjects.Add(original.InstanceId.Value, original);
        selection.SelectInstance(original.InstanceId);
        selection.ApplyRuntimeSelection(null);
        var replacement = new GameObject { InstanceId = original.InstanceId, Name = "Updated" };
        world.GameObjects[original.InstanceId.Value] = replacement;
        world.PublishUpdate();

        Assert.Same(replacement, inspector.SelectedItem);
        Assert.True(selection.SelectedInstanceId.IsEmpty());
    }

    public void Dispose() => MauiProgram.ClearServices();

    sealed class DelayedAsset : AssetFile
    {
        public TaskCompletionSource Ready { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public override Task PrepareInspectorResources() => Ready.Task;
    }
}
