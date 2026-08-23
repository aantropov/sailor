using System.Collections.ObjectModel;
using System.Collections.Specialized;
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
        store.Select("asset-1", SelectionTargetKind.Asset);
        store.Clear();
        store.Clear();

        Assert.Collection(
            snapshots,
            snapshot => Assert.Equal(new SelectionSnapshot("go-root", SelectionTargetKind.GameObject), snapshot),
            snapshot => Assert.Equal(new SelectionSnapshot("cmp-1", SelectionTargetKind.Component), snapshot),
            snapshot => Assert.Equal(new SelectionSnapshot("asset-1", SelectionTargetKind.Asset), snapshot),
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
    public void HierarchyProjection_Flatten_OnlyEmitsVisibleRows_WithDepth()
    {
        var roots = HierarchyProjectionBuilder.Build(
        [
            new HierarchySourceItem("root", "Root", null),
            new HierarchySourceItem("child-a", "Child A", "root"),
            new HierarchySourceItem("child-b", "Child B", "root"),
            new HierarchySourceItem("grandchild", "Grandchild", "child-a")
        ],
        new HashSet<string> { "root" },
        "child-b");

        var rows = HierarchyProjectionBuilder.Flatten(roots);

        Assert.Collection(
            rows,
            row =>
            {
                Assert.Equal("root", row.InstanceId);
                Assert.Equal(0, row.Depth);
                Assert.True(row.HasChildren);
                Assert.True(row.IsExpanded);
            },
            row =>
            {
                Assert.Equal("child-a", row.InstanceId);
                Assert.Equal(1, row.Depth);
                Assert.True(row.HasChildren);
                Assert.False(row.IsExpanded);
            },
            row =>
            {
                Assert.Equal("child-b", row.InstanceId);
                Assert.Equal(1, row.Depth);
                Assert.False(row.HasChildren);
                Assert.True(row.IsSelected);
            });
    }

    [Fact]
    public void HierarchyProjection_PreservesPrefabLinkOnEveryInstanceRow()
    {
        var roots = HierarchyProjectionBuilder.Build(
        [
            new HierarchySourceItem(
                "root",
                "Duck",
                null,
                IsPrefabLinked: true,
                PrefabFileId: "{PREFAB}",
                PrefabRootInstanceId: "root"),
            new HierarchySourceItem(
                "child",
                "Mesh",
                "root",
                IsPrefabLinked: true,
                PrefabFileId: "{PREFAB}",
                PrefabRootInstanceId: "root")
        ],
        new HashSet<string> { "root" },
        null);

        var rows = HierarchyProjectionBuilder.Flatten(roots);

        Assert.True(rows[0].IsPrefabLinked);
        Assert.Equal("{PREFAB}", rows[0].PrefabFileId);
        Assert.Equal("root", rows[0].PrefabRootInstanceId);
        Assert.Equal("◆", rows[0].PrefabLinkGlyph);
        Assert.True(rows[1].IsPrefabLinked);
        Assert.Equal("{PREFAB}", rows[1].PrefabFileId);
        Assert.Equal("root", rows[1].PrefabRootInstanceId);
        Assert.Equal("◆", rows[1].PrefabLinkGlyph);
    }

    [Fact]
    public void HierarchyProjection_NestsLinkedRootUnderExternalParent()
    {
        var roots = HierarchyProjectionBuilder.Build(
        [
            new HierarchySourceItem(
                "external-parent",
                "Parent",
                null),
            new HierarchySourceItem(
                "linked-root",
                "Duck",
                "external-parent",
                IsPrefabLinked: true,
                PrefabFileId: "{PREFAB}")
        ],
        new HashSet<string> { "external-parent" },
        null);

        var rows = HierarchyProjectionBuilder.Flatten(roots);

        Assert.Collection(
            rows,
            row =>
            {
                Assert.Equal("external-parent", row.InstanceId);
                Assert.Equal(0, row.Depth);
            },
            row =>
            {
                Assert.Equal("linked-root", row.InstanceId);
                Assert.Equal(1, row.Depth);
                Assert.True(row.IsPrefabLinked);
            });
    }

    [Fact]
    public void HierarchyRows_ReparentThenUnrelatedRefreshPreservesStableRowsAndSelection()
    {
        var selectedRow = new HierarchyListRow("c", 0, "C", false, false, true);
        var rows = new ObservableCollection<HierarchyListRow>
        {
            new("a", 0, "A", false, false, false),
            new("b", 0, "B", false, false, false),
            selectedRow,
        };
        var actions = new List<NotifyCollectionChangedAction>();
        rows.CollectionChanged += (_, args) => actions.Add(args.Action);

        HierarchyListRow[] reparented =
        [
            new("a", 0, "A", true, true, false),
            new("b", 1, "B", false, false, false),
            new("c", 0, "C", false, false, true),
        ];

        HierarchyRowReconciler.Reconcile(rows, reparented);

        Assert.Equal(3, rows.Count);
        Assert.Equal(1, rows[1].Depth);
        Assert.Same(selectedRow, rows[2]);
        Assert.DoesNotContain(NotifyCollectionChangedAction.Reset, actions);

        actions.Clear();
        HierarchyRowReconciler.Reconcile(rows, [.. reparented]);

        Assert.Empty(actions);
        Assert.Same(selectedRow, rows.Single(row => row.IsSelected));
    }

    [Fact]
    public void ContentRows_AddReorderAndRemoveWithoutResetOrEmptyProjection()
    {
        var first = new ContentProjectionTestRow("a", "A");
        var second = new ContentProjectionTestRow("b", "B");
        var rows = new ObservableCollection<ContentProjectionTestRow>
        {
            first,
            second,
        };
        var actions = new List<NotifyCollectionChangedAction>();
        var observedCounts = new List<int>();
        rows.CollectionChanged += (_, args) =>
        {
            actions.Add(args.Action);
            observedCounts.Add(rows.Count);
        };

        ContentProjectionTestRow[] withPrefab =
        [
            first,
            new("prefab", "Prefab"),
            second,
        ];
        ContentRowReconciler.Reconcile(
            rows,
            withPrefab,
            row => row.Id);

        Assert.Equal(
            ["a", "prefab", "b"],
            rows.Select(row => row.Id));
        Assert.Same(first, rows[0]);
        Assert.Same(second, rows[2]);
        Assert.DoesNotContain(
            NotifyCollectionChangedAction.Reset,
            actions);
        Assert.DoesNotContain(0, observedCounts);

        actions.Clear();
        observedCounts.Clear();
        ContentRowReconciler.Reconcile(
            rows,
            [
                first,
                second,
            ],
            row => row.Id);

        Assert.Equal(
            [NotifyCollectionChangedAction.Remove],
            actions);
        Assert.Equal(
            ["a", "b"],
            rows.Select(row => row.Id));
        Assert.DoesNotContain(0, observedCounts);

        actions.Clear();
        observedCounts.Clear();
        ContentRowReconciler.Reconcile(
            rows,
            [
                new("b", "B updated"),
                new("a", "A"),
            ],
            row => row.Id);

        Assert.Equal(
            ["b", "a"],
            rows.Select(row => row.Id));
        Assert.Equal("B updated", rows[0].Label);
        Assert.DoesNotContain(
            NotifyCollectionChangedAction.Reset,
            actions);
        Assert.DoesNotContain(0, observedCounts);
    }

    sealed record ContentProjectionTestRow(
        string Id,
        string Label);

    [Fact]
    public void InspectorSelectionIdentity_UsesStableValueIdentity()
    {
        Assert.True(InspectorSelectionIdentity.AreEquivalent(
            typeof(object),
            new string("game-object".ToCharArray()),
            typeof(object),
            new string("game-object".ToCharArray())));
        Assert.False(InspectorSelectionIdentity.AreEquivalent(
            typeof(object),
            "game-object",
            typeof(string),
            "game-object"));
        Assert.False(InspectorSelectionIdentity.AreEquivalent(
            typeof(object),
            "game-object",
            typeof(object),
            "another-object"));
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

    [Fact]
    public void InspectorAutoCommitController_IgnoresConfiguredProperties()
    {
        var controller = new InspectorAutoCommitController(
            propertyName => propertyName == "IsDirty",
            propertyName => true);
        controller.MarkInitialized();

        var decision = controller.OnPropertyChanged("IsDirty");

        Assert.Equal(InspectorAutoCommitDecision.None, decision);
    }

    [Fact]
    public void InspectorAutoCommitController_DoesNotMarkDirtyUntilInitialized()
    {
        var controller = new InspectorAutoCommitController(
            propertyName => propertyName == "IsDirty",
            propertyName => propertyName == "OverrideProperties");

        var decision = controller.OnPropertyChanged("OverrideProperties");

        Assert.Equal(InspectorAutoCommitDecision.None, decision);
    }

    [Fact]
    public void InspectorAutoCommitController_MarksDirtyAfterInitialization()
    {
        var controller = new InspectorAutoCommitController(
            propertyName => propertyName == "IsDirty",
            propertyName => propertyName == "OverrideProperties");
        controller.MarkInitialized();

        var decision = controller.OnPropertyChanged("DisplayName");

        Assert.True(decision.MarkDirty);
        Assert.False(decision.CommitNow);
    }

    [Fact]
    public void InspectorAutoCommitController_RequestsImmediateCommitForConfiguredProperty()
    {
        var controller = new InspectorAutoCommitController(
            propertyName => propertyName == "IsDirty",
            propertyName => propertyName == "OverrideProperties");
        controller.MarkInitialized();

        var decision = controller.OnPropertyChanged("OverrideProperties");

        Assert.True(decision.MarkDirty);
        Assert.True(decision.CommitNow);
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(true, true)]
    public void InspectorAutoCommitController_ExplicitCommitRequiresPendingChanges(bool isDirty, bool expected)
    {
        var controller = new InspectorAutoCommitController(
            propertyName => propertyName == "IsDirty",
            propertyName => true);
        controller.MarkInitialized();

        Assert.Equal(expected, controller.ShouldCommitPendingChanges(isDirty));
    }
}
