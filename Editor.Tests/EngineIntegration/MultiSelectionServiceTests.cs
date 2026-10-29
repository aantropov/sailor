using CommunityToolkit.Mvvm.ComponentModel;
using SailorEditor;
using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEngine;

namespace Editor.Tests.EngineIntegration;

[Collection("LocalEngineProtocolTransport")]
public sealed class MultiSelectionServiceTests : IDisposable
{
    readonly SelectionService selection = new();
    readonly WorldService world = new();

    public MultiSelectionServiceTests() => MauiProgram.SetService(world);

    [Fact]
    public void GroupSceneSelectionPreservesEveryIdAndTheLastPrimary()
    {
        var first = Add("first");
        var second = Add("second");
        selection.SelectInstances([first.InstanceId, second.InstanceId]);
        Assert.Equal(new[] { "first", "second" }, selection.Snapshot.SelectedIds);
        Assert.Equal(new[] { "first", "second" }, selection.SelectedInstanceIds.Select(id => id.Value));
        Assert.Same(second, selection.SelectedItem);
        Assert.Same(second, selection.InspectedItem);
        Assert.Equal(2, selection.SelectedItems.Count);
        Assert.Equal(2, selection.InspectedItems.Count);
    }

    [Fact]
    public void RefreshRebindsEveryModelAndDropsOnlyDeletedMembers()
    {
        var first = Add("first");
        var second = Add("second");
        var third = Add("third");
        selection.SelectInstances([first.InstanceId, second.InstanceId, third.InstanceId]);
        var replacement = new GameObject { InstanceId = first.InstanceId, Name = "Replacement" };
        world.GameObjects["first"] = replacement;
        world.GameObjects.Remove("second");
        selection.RebindSceneSelection();
        Assert.Equal(new[] { "first", "third" }, selection.Snapshot.SelectedIds);
        Assert.Same(replacement, selection.SelectedItems[0]);
        Assert.Same(third, selection.SelectedItem);
        Assert.Equal(2, selection.InspectedItems.Count);
    }

    [Fact]
    public void EmptySceneClickRetainsTheGroupInspectorWithoutReselectingItDuringRefresh()
    {
        var first = Add("first");
        var second = Add("second");
        selection.SelectInstances([first.InstanceId, second.InstanceId]);
        selection.ApplyRuntimeSelection(null);
        Assert.Empty(selection.SelectedItems);
        Assert.Empty(selection.SelectedInstanceIds);
        Assert.Equal(2, selection.InspectedItems.Count);
        var replacement = new GameObject { InstanceId = second.InstanceId };
        world.GameObjects["second"] = replacement;
        selection.RebindSceneSelection();
        Assert.Empty(selection.SelectedItems);
        Assert.Same(replacement, selection.InspectedItems[1]);
    }

    [Fact]
    public async Task NewSceneGroupSupersedesAnAssetGroupThatIsStillLoading()
    {
        var first = new DelayedAsset { FileId = new("asset-a"), IsMetadataLoaded = true };
        var second = new AssetFile { FileId = new("asset-b"), IsMetadataLoaded = true };
        var pending = selection.SelectObjectsAsync([first, second]);
        Assert.False(pending.IsCompleted);
        var one = Add("one");
        var two = Add("two");
        selection.SelectInstances([one.InstanceId, two.InstanceId]);
        first.Ready.SetResult(true);
        await pending;
        Assert.Equal(new[] { "one", "two" }, selection.Snapshot.SelectedIds);
        Assert.Same(two, selection.SelectedItem);
        Assert.All(selection.InspectedItems, item => Assert.IsType<GameObject>(item));
    }

    [Fact]
    public async Task ReselectingDisplayedGroupCancelsPendingAssetPreview()
    {
        var first = Add("first");
        var second = Add("second");
        await selection.SelectObjectsAsync([first, second]);
        var asset = new DelayedPreviewAsset { FileId = new("asset"), IsMetadataLoaded = true };
        var pending = selection.SelectObjectAsync(asset);
        Assert.False(pending.IsCompleted);
        Assert.Equal("asset", selection.Snapshot.SelectedId);
        Assert.Same(second, selection.SelectedItem);

        await selection.SelectObjectsAsync([first, second]);
        asset.Ready.SetResult();
        await pending;

        Assert.Equal(new[] { "first", "second" }, selection.Snapshot.SelectedIds);
        Assert.Same(second, selection.SelectedItem);
        Assert.Equal(new ObservableObject[] { first, second }, selection.InspectedItems);
    }

    [Fact]
    public async Task WorkspaceResetCancelsPendingGroupPublication()
    {
        var first = new DelayedAsset { FileId = new("asset-a"), IsMetadataLoaded = true };
        var second = new AssetFile { FileId = new("asset-b"), IsMetadataLoaded = true };
        var pending = selection.SelectObjectsAsync([first, second]);
        var epoch = selection.WorkspaceEpoch;
        selection.ResetForWorkspaceChange();
        first.Ready.SetResult(true);
        await pending;
        Assert.True(selection.WorkspaceEpoch > epoch);
        Assert.Empty(selection.Snapshot.SelectedIds);
        Assert.Empty(selection.SelectedItems);
        Assert.Empty(selection.InspectedItems);
        selection.CompleteWorkspaceChange();
    }

    GameObject Add(string id)
    {
        var item = new GameObject { InstanceId = new(id), Name = id };
        world.GameObjects[id] = item;
        return item;
    }

    public void Dispose() => MauiProgram.ClearServices();

    sealed class DelayedPreviewAsset : AssetFile
    {
        public TaskCompletionSource Ready { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public override Task PrepareInspectorResources() => Ready.Task;
    }

    sealed class DelayedAsset : AssetFile
    {
        public TaskCompletionSource<bool> Ready { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);
        public override Task<bool> LoadDependentResources() => Ready.Task;
    }
}
