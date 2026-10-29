using SailorEditor.Workflow;

namespace Editor.Tests;

public sealed class MultiSelectionStateTests
{
    [Fact]
    public void GroupSelectionPreservesOrderAndTheLastPrimary()
    {
        var store = new SelectionStore();
        Assert.True(store.SelectMany(["a", "b", "c"], SelectionTargetKind.GameObject));
        Assert.Equal(new[] { "a", "b", "c" }, store.Current.SelectedIds);
        Assert.Equal("c", store.Current.SelectedId);
        Assert.False(store.SelectMany(["a", "b", "c"], SelectionTargetKind.GameObject));
        Assert.True(store.SelectMany(["b", "c"], SelectionTargetKind.GameObject));
        Assert.Equal("c", store.Current.SelectedId);
    }

    [Fact]
    public void SnapshotRetainsItsOwnIdsWhenTheCallerOrSelectionChanges()
    {
        var store = new SelectionStore();
        var ids = new List<string> { "a", "b" };
        store.SelectMany(ids, SelectionTargetKind.Asset);
        var retained = store.Current;
        ids.Clear();
        store.SelectMany(["c"], SelectionTargetKind.Asset);
        Assert.Equal(new[] { "a", "b" }, retained.SelectedIds);
        Assert.Equal("b", retained.SelectedId);
        Assert.NotEqual(retained, store.Current);
    }

    [Fact]
    public void MembershipChangesNotifyEvenWhenThePrimaryDoesNotChange()
    {
        var store = new SelectionStore();
        var notifications = new List<SelectionSnapshot>();
        store.Changed += notifications.Add;
        store.SelectMany(["a", "c"], SelectionTargetKind.GameObject);
        store.SelectMany(["a", "b", "c"], SelectionTargetKind.GameObject);
        Assert.Equal(2, notifications.Count);
        Assert.All(notifications, snapshot => Assert.Equal("c", snapshot.SelectedId));
        store.Clear();
        Assert.Empty(store.Current.SelectedIds);
        Assert.Equal(SelectionSnapshot.Empty, store.Current);
    }

    [Fact]
    public void RepeatedIdsKeepTheMostRecentPrimaryAndEmptyIdsAreIgnored()
    {
        var store = new SelectionStore();
        store.SelectMany(["a", "", "b", "a", " "], SelectionTargetKind.GameObject);
        Assert.Equal(new[] { "b", "a" }, store.Current.SelectedIds);
        Assert.Equal("a", store.Current.SelectedId);
        var equivalent = new SelectionSnapshot("a", SelectionTargetKind.GameObject)
        {
            SelectedIds = new[] { "b", "a" }
        };
        Assert.Equal(equivalent, store.Current);
        Assert.Equal(equivalent.GetHashCode(), store.Current.GetHashCode());
    }

    [Fact]
    public void HierarchyHighlightsAllSelectedIdsAcrossCollapsedAndExpandedRows()
    {
        HierarchySourceItem[] items = [
            new("root", "Root", null),
            new("child", "Child", "root"),
            new("other", "Other", null)
        ];
        var collapsed = HierarchyProjectionBuilder.Flatten(
            HierarchyProjectionBuilder.BuildSelection(items, new HashSet<string>(), ["child", "other"]));
        Assert.Single(collapsed, row => row.IsSelected);
        var expanded = HierarchyProjectionBuilder.Flatten(
            HierarchyProjectionBuilder.BuildSelection(items, new HashSet<string> { "root" }, ["child", "other"]));
        Assert.Equal(new[] { "other", "child" }, expanded.Where(row => row.IsSelected).Select(row => row.InstanceId));
        Assert.False(expanded.Single(row => row.InstanceId == "root").IsSelected);
    }
}
