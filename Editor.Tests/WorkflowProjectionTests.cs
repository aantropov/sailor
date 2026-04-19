using SailorEditor.Workflow;

namespace Editor.Tests;

public sealed class WorkflowProjectionTests
{
    [Fact]
    public void SelectionStore_OnlyRaisesWhenSnapshotChanges()
    {
        var store = new SelectionStore();
        var snapshots = new List<SelectionSnapshot>();
        store.Changed += snapshots.Add;

        store.Select("go-root", SelectionTargetKind.GameObject);
        store.Select("go-root", SelectionTargetKind.GameObject);
        store.Select("cmp-1", SelectionTargetKind.Component);
        store.Clear();
        store.Clear();

        Assert.Collection(
            snapshots,
            snapshot => Assert.Equal(new SelectionSnapshot("go-root", SelectionTargetKind.GameObject), snapshot),
            snapshot => Assert.Equal(new SelectionSnapshot("cmp-1", SelectionTargetKind.Component), snapshot),
            snapshot => Assert.Equal(SelectionSnapshot.Empty, snapshot));
    }

    [Fact]
    public void HierarchyProjection_BuildsNestedTree_AndMarksSelectionAndExpansion()
    {
        var roots = HierarchyProjectionBuilder.Build(
        [
            new HierarchySourceItem("b", "Bravo", null),
            new HierarchySourceItem("a", "Alpha", null),
            new HierarchySourceItem("a-child", "Child", "a")
        ],
        new HashSet<string> { "a" },
        "a-child");

        Assert.Equal(2, roots.Count);
        Assert.Equal("Alpha", roots[0].Label);
        Assert.True(roots[0].IsExpanded);
        Assert.Single(roots[0].Children);
        Assert.True(roots[0].Children[0].IsSelected);
        Assert.Equal("Bravo", roots[1].Label);
    }

    [Fact]
    public void InspectorProjection_UsesEmptyProjectionForNoSelection()
    {
        var empty = InspectorProjectionBuilder.Build(null, SelectionTargetKind.None, null, null, null);
        var selected = InspectorProjectionBuilder.Build(
            "go-root",
            SelectionTargetKind.GameObject,
            "Root",
            "go-root",
            [new InspectorComponentProjection("cmp-1", "Transform", "TransformComponent")]);

        Assert.Equal(InspectorProjection.Empty, empty);
        Assert.Equal("go-root", selected.SelectedId);
        Assert.Single(selected.Components);
        Assert.Equal("Transform", selected.Components[0].DisplayName);
    }
}
