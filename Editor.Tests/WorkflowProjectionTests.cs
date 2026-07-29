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
    public void HierarchyBreakPrefabLink_UsesProjectedRootForChildRows()
    {
        var hierarchySource = ReadRepositoryFile(
            "Editor",
            "Views",
            "HierarchyView.xaml.cs");
        var breakLinkMenu = Slice(
            hierarchySource,
            "if (row is",
            "return contextMenu.CreateFlyout([.. items]);");

        Assert.Contains(
            "PrefabRootInstanceId: not null",
            breakLinkMenu,
            StringComparison.Ordinal);
        Assert.Contains(
            "new InstanceId(",
            breakLinkMenu,
            StringComparison.Ordinal);
        Assert.Contains(
            "row.PrefabRootInstanceId",
            breakLinkMenu,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "gameObject.InstanceId,",
            breakLinkMenu,
            StringComparison.Ordinal);
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
    public void WorldPopulation_PreservesPrefabLinkMetadataAndCloneIsolation()
    {
        var worldServiceSource = ReadRepositoryFile(
            "Editor",
            "Services",
            "WorldService.cs");
        var prefabSource = ReadRepositoryFile(
            "Editor",
            "ViewModels",
            "Prefab.cs");
        var projectionSource = ReadRepositoryFile(
            "Editor",
            "Services",
            "HierarchyProjectionService.cs");
        var populate = Slice(
            worldServiceSource,
            "public bool TryPopulateWorld(",
            "void PublishCurrentWorld()");

        AssertInOrder(
            populate,
            "var newPrefab = (Prefab)prefab.Clone();",
            "newPrefab.GameObjects.Clear();",
            "newPrefab.Components.Clear();",
            "Current.Prefabs.Add(newPrefab);");
        Assert.DoesNotContain(
            "var newPrefab = new Prefab();",
            populate,
            StringComparison.Ordinal);

        Assert.Contains(
            "FileId = FileId is null",
            prefabSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "ParentInstanceId = ParentInstanceId",
            prefabSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "new Dictionary<string, string>(",
            prefabSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "GameObjectOverrides = GameObjectOverrides?",
            prefabSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "ComponentOverrides = ComponentOverrides?",
            prefabSource,
            StringComparison.Ordinal);
        Assert.Contains(
            ": (Vec4)Position.Clone()",
            prefabSource,
            StringComparison.Ordinal);
        Assert.Contains(
            ": (Rotation)Rotation.Clone()",
            prefabSource,
            StringComparison.Ordinal);
        Assert.Contains(
            ": (Vec4)Scale.Clone()",
            prefabSource,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "Position = Position,",
            prefabSource,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "Rotation = Rotation,",
            prefabSource,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "Scale = Scale,",
            prefabSource,
            StringComparison.Ordinal);

        Assert.Contains(
            "prefab.ParentInstanceId",
            worldServiceSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "ResolveParentInstanceId(",
            projectionSource,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "static string? ResolveParentId(",
            projectionSource,
            StringComparison.Ordinal);
        Assert.True(
            CountOccurrences(projectionSource, "x.prefab.FileId") >= 4,
            "Hierarchy projection must carry the linked prefab id to every source row.");
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

    [Fact]
    public void InspectorViewModels_DeferGameObjectCommitUntilEditorCompletion()
    {
        var gameObjectSource = ReadRepositoryFile("Editor", "ViewModels", "GameObject.cs");
        var componentSource = ReadRepositoryFile("Editor", "ViewModels", "Component.cs");
        var gameObjectTemplateSource = ReadRepositoryFile("Editor", "Views", "InspectorView", "GameObjectTemplate.xaml");

        Assert.Contains("propertyName => false", gameObjectSource, StringComparison.Ordinal);
        Assert.Contains("Completed=\"OnEntryCompleted\"", gameObjectTemplateSource, StringComparison.Ordinal);
        Assert.Contains("Unfocused=\"OnEntryUnfocused\"", gameObjectTemplateSource, StringComparison.Ordinal);
        Assert.Contains(
            "propertyName => propertyName == nameof(OverrideProperties)",
            componentSource,
            StringComparison.Ordinal);

        AssertCommitClearsDirtyBeforeDispatch(gameObjectSource);
        AssertCommitClearsDirtyBeforeDispatch(componentSource);
        AssertCommitSkipsUnchangedYaml(gameObjectSource);
        AssertCommitSkipsUnchangedYaml(componentSource);
    }

    [Fact]
    public void WorldProjectionRefresh_DropsStaleSelectionBeforePublishing()
    {
        var worldSource = ReadRepositoryFile("Editor", "Services", "WorldService.cs");
        var inspectorSource = ReadRepositoryFile("Editor", "Services", "InspectorProjectionService.cs");
        var gameObjectTemplateSource = ReadRepositoryFile("Editor", "Views", "InspectorView", "GameObjectTemplate.xaml.cs");
        var publish = Slice(worldSource, "void PublishCurrentWorld()", "void RefreshSelection()");
        var getComponents = Slice(worldSource, "public List<Component> GetComponents", "public Component FindComponent");
        var resolveSelection = Slice(inspectorSource, "object? ResolveSelectedItem()", "GameObject? ResolveOwner");

        AssertInOrder(publish, "RefreshSelection();", "OnUpdateWorldAction?.Invoke(Current)");
        Assert.Contains("componentIndex < 0 || componentIndex >= prefab.Components.Count", getComponents, StringComparison.Ordinal);
        AssertInOrder(
            resolveSelection,
            "TryGetGameObject(selectedInstanceId",
            "TryGetComponent(selectedInstanceId",
            "return null;");
        Assert.Contains("DispatchDelayed", gameObjectTemplateSource, StringComparison.Ordinal);
        Assert.DoesNotContain("CommitFrom(sender)", gameObjectTemplateSource, StringComparison.Ordinal);
    }

    [Fact]
    public void ComponentMutations_StayOnTheCallingThreadUntilProjectionRefreshCompletes()
    {
        var source = ReadRepositoryFile("Editor", "ViewModels", "GameObject.cs");
        var mutationMethods = Slice(
            source,
            "public async Task AddComponentFromInspectorAsync()",
            "[ObservableProperty]");

        Assert.DoesNotContain("Task.Run", mutationMethods, StringComparison.Ordinal);
        Assert.DoesNotContain("async void ClearComponentsFromInspector", mutationMethods, StringComparison.Ordinal);
        Assert.DoesNotContain("new Command(async", source, StringComparison.Ordinal);
        Assert.Contains(
            "new AsyncRelayCommand(AddComponentFromInspectorAsync)",
            source,
            StringComparison.Ordinal);
        Assert.Contains(
            "new AsyncRelayCommand(ClearComponentsFromInspectorAsync)",
            source,
            StringComparison.Ordinal);
        AssertInOrder(
            mutationMethods,
            "ShowAsync(",
            "AddComponentAsync(this, componentTypeName)",
            "public async Task ClearComponentsFromInspectorAsync()",
            "RemoveComponentAsync(component)");
    }

    [Fact]
    public void DestroyGameObjectUndo_UsesAnInMemoryPrefabSnapshot()
    {
        var source = ReadRepositoryFile("Editor", "Commands", "EditorWorldCommands.cs");
        var snapshot = Slice(
            source,
            "sealed class CreatedHierarchySnapshot",
            "static class OwnedHierarchyRollback");
        var destroyCommand = Slice(
            source,
            "public sealed class DestroyGameObjectCommand",
            "public sealed class ReparentGameObjectCommand");

        Assert.DoesNotContain("CreatePrefabAsset", destroyCommand, StringComparison.Ordinal);
        Assert.Contains(
            "CreatedHierarchySnapshot? _snapshot",
            destroyCommand,
            StringComparison.Ordinal);
        Assert.Contains(
            "CreatedHierarchySnapshot.TryCapture(",
            destroyCommand,
            StringComparison.Ordinal);
        Assert.Contains(
            "StrictHierarchyRestore.RestoreAsync(",
            destroyCommand,
            StringComparison.Ordinal);
        Assert.Contains(
            "CreatePrefabFromSubHierarchy",
            snapshot,
            StringComparison.Ordinal);
        Assert.Contains(
            "EditorYaml.SerializePrefab",
            snapshot,
            StringComparison.Ordinal);
        Assert.Contains("_gameObjectIds", snapshot, StringComparison.Ordinal);
        Assert.Contains("_componentIds", snapshot, StringComparison.Ordinal);
    }

    [Fact]
    public void DestroyGameObjectUndo_IsAtomicAndRollsBackOnlyExactIdentity()
    {
        var source = ReadRepositoryFile(
            "Editor",
            "Commands",
            "EditorWorldCommands.cs");
        var snapshot = Slice(
            source,
            "sealed class CreatedHierarchySnapshot",
            "static class OwnedHierarchyRollback");
        var ownedRollback = Slice(
            source,
            "static class OwnedHierarchyRollback",
            "static class StrictHierarchyRestore");
        var strictRestore = Slice(
            source,
            "static class StrictHierarchyRestore",
            "sealed class CreatedHierarchyCommandState");
        var destroyCommand = Slice(
            source,
            "public sealed class DestroyGameObjectCommand",
            "public sealed class ReparentGameObjectCommand");

        Assert.DoesNotContain(
            "beforeIds",
            destroyCommand,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "SelectMany(",
            destroyCommand,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "FirstOrDefault(",
            destroyCommand,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "restored.InstanceId",
            destroyCommand,
            StringComparison.Ordinal);
        AssertInOrder(
            destroyCommand,
            "cancellationToken.ThrowIfCancellationRequested();",
            "RefreshAndClassifyAsync(",
            "AuthoritativeHierarchyProjection.Exact",
            "RequestDestroyObjectAsync(",
            "CancellationToken.None",
            "HierarchyMutationRecoveryPolicy.ResolveDestroy(",
            "RecoverySuccess(");
        Assert.Contains(
            "ResolveLinkedRootPrefabFileId(gameObject)",
            destroyCommand,
            StringComparison.Ordinal);
        AssertInOrder(
            destroyCommand,
            "gameObject.ParentIndex != uint.MaxValue",
            "var prefabFileId = prefab.FileId",
            "new FileId(prefabFileId.Value)");
        AssertInOrder(
            strictRestore,
            "RefreshCurrentWorldAuthoritativelyAsync(",
            "snapshot.ClassifyProjection(world, parentId)",
            "AuthoritativeHierarchyProjection.Absent",
            "RequestInstantiatePrefabFromYamlStrictAsync(",
            "CancellationToken.None",
            "RefreshCurrentWorldAuthoritativelyAsync(",
            "HierarchyMutationRecoveryPolicy.ResolveStrictRestore(",
            "if (resolution.RollbackOwnedHierarchy)",
            "OwnedHierarchyRollback.FailureAsync(");
        Assert.Contains(
            "catch (Exception exception)",
            destroyCommand,
            StringComparison.Ordinal);
        Assert.Contains(
            "Destroy outcome is uncertain; the undo entry is retained for recovery",
            destroyCommand,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "RequestDestroyObjectAsync(\n                    _activeInstanceId,\n                    cancellationToken",
            destroyCommand,
            StringComparison.Ordinal);
        Assert.Contains(
            "DestroyObjectAsync(\n                rootInstanceId,",
            ownedRollback,
            StringComparison.Ordinal);
        Assert.Contains(
            "RefreshCurrentWorldAsync(",
            ownedRollback,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "SelectMany(",
            ownedRollback,
            StringComparison.Ordinal);
        Assert.Contains(
            "MatchesProjection(world, expectedParentId)",
            snapshot,
            StringComparison.Ordinal);
        Assert.Contains(
            "EditorYaml.SerializePrefab(projectedPrefab)",
            snapshot,
            StringComparison.Ordinal);
    }

    [Fact]
    public void DestroyGameObjectUndo_RejectsRootIdentityCollisionBeforeNativeMutation()
    {
        var source = ReadRepositoryFile(
            "Editor",
            "Commands",
            "EditorWorldCommands.cs");
        var destroyCommand = Slice(
            source,
            "public sealed class DestroyGameObjectCommand",
            "public sealed class ReparentGameObjectCommand");
        var strictRestore = Slice(
            source,
            "static class StrictHierarchyRestore",
            "sealed class CreatedHierarchyCommandState");
        var collisionGuard = Slice(
            strictRestore,
            "if (snapshot.ClassifyProjection(world, parentId) !=",
            "// EditorCommandDispatcher serializes managed mutations.");

        Assert.Contains(
            "saved instance ids are already in use",
            collisionGuard,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "RequestInstantiatePrefabFromYamlStrictAsync(",
            collisionGuard,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "DestroyObjectAsync(",
            collisionGuard,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "OwnedHierarchyRollback.FailureAsync(",
            collisionGuard,
            StringComparison.Ordinal);
        AssertInOrder(
            strictRestore,
            "RefreshCurrentWorldAuthoritativelyAsync(",
            "snapshot.ClassifyProjection(world, parentId)",
            "saved instance ids are already in use",
            "// EditorCommandDispatcher serializes managed mutations.",
            "RequestInstantiatePrefabFromYamlStrictAsync(");
        AssertInOrder(
            destroyCommand,
            "cancellationToken.ThrowIfCancellationRequested();",
            "RefreshAndClassifyAsync(",
            "AuthoritativeHierarchyProjection.Exact",
            "ResolveDestroyUndo(",
            "DestroyHierarchyUndoAction.CompleteWithoutMutation",
            "DestroyHierarchyUndoAction.FailWithoutMutation",
            "StrictHierarchyRestore.RestoreAsync(");
    }

    [Fact]
    public void ExternalProjectedParent_IsPreservedByDeleteUndoAndReparentToRoot()
    {
        var commandSource = ReadRepositoryFile(
            "Editor",
            "Commands",
            "EditorWorldCommands.cs");
        var worldSource = ReadRepositoryFile(
            "Editor",
            "Services",
            "WorldService.cs");
        var destroyCommand = Slice(
            commandSource,
            "public sealed class DestroyGameObjectCommand",
            "public sealed class ReparentGameObjectCommand");
        var hierarchySnapshot = Slice(
            commandSource,
            "sealed class CreatedHierarchySnapshot",
            "static class OwnedHierarchyRollback");
        var reparentCommand = Slice(
            commandSource,
            "public sealed class ReparentGameObjectCommand",
            "public sealed class AddComponentCommand");
        var parentResolver = Slice(
            worldSource,
            "public InstanceId? ResolveParentInstanceId(",
            "public List<Component> GetComponents(");
        var reparentToRoot = Slice(
            worldSource,
            "public async Task<bool> ReparentToRootAsync(",
            "public bool ApplyReparentLocal(");

        AssertInOrder(
            parentResolver,
            "if (gameObject.ParentIndex != uint.MaxValue)",
            "prefab.GameObjects[(int)gameObject.ParentIndex].InstanceId",
            "prefab.ParentInstanceId",
            "new InstanceId(prefab.ParentInstanceId)");
        Assert.Contains(
            ".ResolveParentInstanceId(gameObject)",
            destroyCommand,
            StringComparison.Ordinal);
        AssertInOrder(
            hierarchySnapshot,
            "var linkedParentPrefab =",
            "linkedParentPrefab.FileId",
            "!ContainsMappedInstanceId(linkedParentPrefab, parentId)",
            "ContainsMappedInstanceId(",
            "root.InstanceId",
            "prefab.DetachedFromPrefab = true;",
            "prefab.ParentInstanceId = parentId.Value;");
        AssertInOrder(
            destroyCommand,
            "_snapshot.DetachedFromPrefab",
            "? null",
            ": _prefabLinkFileId");
        Assert.Contains(
            ".ResolveParentInstanceId(child)",
            reparentCommand,
            StringComparison.Ordinal);
        Assert.Contains(
            "ResolveParentInstanceId(child) is null",
            reparentToRoot,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "child.ParentIndex == uint.MaxValue",
            reparentToRoot,
            StringComparison.Ordinal);
    }

    [Fact]
    public void PrefabSnapshotInterop_UsesTypedProtocolRequest()
    {
        var serviceSource = ReadRepositoryFile("Editor", "Services", "EngineService.cs");
        var clientSource = ReadRepositoryFile("Editor", "Protocol", "EngineProtocolClient.cs");

        Assert.Contains(
            "protocolClient.InstantiatePrefabFromYamlAsync(",
            serviceSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "InstantiatePrefabFromYaml =",
            clientSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "new InstantiatePrefabFromYamlRequest",
            clientSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "PrefabYaml = ValidateString(",
            clientSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "nameof(prefabYaml)",
            clientSource,
            StringComparison.Ordinal);
        Assert.DoesNotContain("MarshalAs", serviceSource, StringComparison.Ordinal);
    }

    [Fact]
    public void LinkedPrefabSnapshotRestore_IsStrictSourceResolvedAndSkipsRelink()
    {
        var commandSource = ReadRepositoryFile(
            "Editor",
            "Commands",
            "EditorWorldCommands.cs");
        var interopSource = ReadRepositoryFile(
            "Runtime",
            "Editor",
            "EditorInterop.cpp");
        var worldSource = ReadRepositoryFile(
            "Runtime",
            "Engine",
            "World.cpp");
        var snapshot = Slice(
            commandSource,
            "sealed class CreatedHierarchySnapshot",
            "static class OwnedHierarchyRollback");
        var strictInterop = Slice(
            interopSource,
            "bool App::InstantiateEditorPrefabFromYaml(",
            "bool App::FocusEditorCamera(");
        var instantiate = Slice(
            worldSource,
            "GameObjectPtr World::Instantiate(PrefabPtr prefab, bool bStrictInstanceIds)",
            "void World::ResolveExternalDependencies()");

        AssertInOrder(
            snapshot,
            "root.ParentIndex == uint.MaxValue",
            "linkedRootPrefab.FileId",
            "ContainsMappedInstanceId(",
            "prefab.FileId =",
            "prefab.ParentInstanceId = parentId?.Value;",
            "prefab.InstanceIds =",
            "prefab.GameObjectOverrides =",
            "prefab.ComponentOverrides =",
            "prefab.LinkedPrefabSnapshot = true;");
        Assert.Contains(
            "snapshot.DetachedFromPrefab ||\n                snapshot.LinkedPrefabSnapshot",
            commandSource,
            StringComparison.Ordinal);

        AssertInOrder(
            strictInterop,
            "if (prefab->IsLinkedPrefabSnapshotRecord())",
            "if (!bStrictInstanceIds ||",
            "prefab->GetLinkedParentInstanceId() !=",
            "prefab->ValidateForInstantiation(",
            "prefabImporter->LoadPrefab_Immediate(",
            "linkedPrefab->ConfigureLinkedInstance(",
            "linkedPrefab->AppendDetachedSupplementalHierarchy(",
            "prefab = std::move(linkedPrefab);",
            "return editor->InstantiatePrefab(");
        AssertInOrder(
            instantiate,
            "if (prefab->m_bLinkedPrefabSnapshotRecord)",
            "Cannot instantiate linked prefab snapshot directly",
            "return {};");
    }

    [Fact]
    public void CreationAndComponentUndoRedo_PreserveIdentityAndProjectionState()
    {
        var commandSource = ReadRepositoryFile("Editor", "Commands", "EditorWorldCommands.cs");
        var engineSource = ReadRepositoryFile("Editor", "Services", "EngineService.cs");
        var createCommand = Slice(
            commandSource,
            "public sealed class CreateGameObjectCommand",
            "public sealed class DestroyGameObjectCommand");
        var addCommand = Slice(
            commandSource,
            "public sealed class AddComponentCommand",
            "public sealed class RemoveComponentCommand");
        var removeCommand = Slice(
            commandSource,
            "public sealed class RemoveComponentCommand",
            "public sealed class ResetComponentToDefaultsCommand");
        var refresh = Slice(
            engineSource,
            "public async Task RefreshCurrentWorldAsync(",
            "async Task<InstanceId?> InvokeCreationInteropAsync");

        AssertInOrder(
            createCommand,
            "await engine.CreateGameObjectAsync(",
            "if (createdId is null)",
            "_createdId = createdId;",
            "TryGetGameObject(createdId",
            "DestroyObjectAsync(",
            "SelectInstance(createdId)");
        AssertInOrder(
            addCommand,
            "await engine.AddComponentAsync(",
            "if (createdId is null)",
            "_componentId = createdId;",
            "TryGetComponent(createdId",
            "RemoveComponentAsync(");
        AssertInOrder(
            removeCommand,
            "await engine.AddComponentAsync(",
            "CommitChangesAsync(",
            "ApplyComponentYamlLocal(restored.InstanceId, _beforeYaml)",
            "_activeInstanceId = restoredInstanceId;");
        Assert.DoesNotContain("QueueWorldUpdate", refresh, StringComparison.Ordinal);
        Assert.Contains("MainThread.InvokeOnMainThreadAsync", refresh, StringComparison.Ordinal);
        Assert.Contains("await MainThread.InvokeOnMainThreadAsync", refresh, StringComparison.Ordinal);
        Assert.DoesNotContain("GetAwaiter().GetResult()", refresh, StringComparison.Ordinal);
    }

    [Fact]
    public void ModelAndPrefabDrops_AreAtomicUndoableHierarchyCommands()
    {
        var source = ReadRepositoryFile(
            "Editor",
            "Commands",
            "EditorWorldCommands.cs");
        var sharedState = Slice(
            source,
            "sealed class CreatedHierarchySnapshot",
            "public sealed class CreateModelGameObjectCommand");
        var modelCommand = Slice(
            source,
            "public sealed class CreateModelGameObjectCommand",
            "public sealed class InstantiatePrefabAssetCommand");
        var prefabCommand = Slice(
            source,
            "public sealed class InstantiatePrefabAssetCommand",
            "public sealed class FocusEditorCameraCommand");

        Assert.Contains(
            ": IUndoableEditorCommand",
            modelCommand,
            StringComparison.Ordinal);
        Assert.Contains(
            ": IUndoableEditorCommand",
            prefabCommand,
            StringComparison.Ordinal);
        Assert.Contains(
            "CreatePrefabFromSubHierarchy",
            sharedState,
            StringComparison.Ordinal);
        Assert.Contains(
            "EditorYaml.SerializePrefab",
            sharedState,
            StringComparison.Ordinal);
        Assert.Contains(
            "snapshot.ClassifyProjection(world, parentId)",
            sharedState,
            StringComparison.Ordinal);
        Assert.Contains(
            "RequestInstantiatePrefabFromYamlStrictAsync(",
            sharedState,
            StringComparison.Ordinal);
        Assert.Contains(
            "RefreshCurrentWorldAuthoritativelyAsync(",
            sharedState,
            StringComparison.Ordinal);
        Assert.Contains(
            "MatchesProjection(world, expectedParentId)",
            sharedState,
            StringComparison.Ordinal);
        Assert.Contains(
            "SetPrefabLinkAsync(",
            sharedState,
            StringComparison.Ordinal);
        Assert.Contains(
            "catch (Exception exception)",
            sharedState,
            StringComparison.Ordinal);
        Assert.Contains(
            "OwnedHierarchyRollback.FailureAsync(",
            sharedState,
            StringComparison.Ordinal);
        Assert.Contains(
            "DestroyObjectAsync(",
            sharedState,
            StringComparison.Ordinal);
        Assert.Contains(
            "CancellationToken.None",
            sharedState,
            StringComparison.Ordinal);
        Assert.Contains(
            "SelectInstance(snapshot.RootInstanceId)",
            sharedState,
            StringComparison.Ordinal);
        Assert.Contains(
            "public ValueTask<CommandResult> UndoAsync(",
            modelCommand,
            StringComparison.Ordinal);
        Assert.Contains(
            "public ValueTask<CommandResult> UndoAsync(",
            prefabCommand,
            StringComparison.Ordinal);
    }

    [Fact]
    public void PrefabAssetCreation_RollsBackFilesAndPossibleLinkOnEveryFailure()
    {
        var source = ReadRepositoryFile(
            "Editor",
            "Commands",
            "EditorAssetCommands.cs");
        var command = Slice(
            source,
            "public sealed class CreatePrefabAssetCommand",
            "static class AssetCommandMessages");

        AssertInOrder(
            command,
            "cancellationToken.ThrowIfCancellationRequested();",
            "var atomicCancellationToken = CancellationToken.None;",
            "BeginCreatePrefabAsset(",
            "try",
            "linkAttempted = true;",
            "SetPrefabLinkAsync(",
            "IsProjectedLinkedPrefab(",
            "CompletePrefabAssetWrite(",
            "catch (Exception exception)",
            "RollbackAsync(");
        Assert.Contains(
            "cleanupPossibleLink: linkAttempted || linkEstablished",
            command,
            StringComparison.Ordinal);
        Assert.Contains(
            "creation.Transaction.IsActive",
            command,
            StringComparison.Ordinal);
        Assert.Contains(
            "engine.BreakPrefabLinkAsync(",
            command,
            StringComparison.Ordinal);
        Assert.True(
            CountOccurrences(
                command,
                "engine.RequestAssetReloadAsync(") >= 2);
        Assert.True(
            CountOccurrences(
                command,
                "catch (Exception exception)") >= 4);
    }

    [Fact]
    public void BreakPrefabLink_IsAtomicAndVerifiesBothHistoryStates()
    {
        var source = ReadRepositoryFile(
            "Editor",
            "Commands",
            "EditorWorldCommands.cs");
        var command = Slice(
            source,
            "public sealed class BreakPrefabLinkCommand",
            "public sealed class DestroyGameObjectCommand");

        AssertInOrder(
            command,
            "cancellationToken.ThrowIfCancellationRequested();",
            "engine.BreakPrefabLinkAsync(",
            "CancellationToken.None",
            "IsProjectedUnlinked(");
        Assert.Contains(
            "RestoreLinkAsync(",
            command,
            StringComparison.Ordinal);
        Assert.Contains(
            "IsProjectedLinked(",
            command,
            StringComparison.Ordinal);
        Assert.Contains(
            "RestoreUnlinkedAsync(",
            command,
            StringComparison.Ordinal);
        Assert.Contains(
            "RefreshProjectionSafelyAsync(",
            command,
            StringComparison.Ordinal);
    }

    [Fact]
    public void PrefabCreationAndHierarchyFocus_CommitPendingInspectorEditsFirst()
    {
        var contentSource = ReadRepositoryFile(
            "Editor",
            "Views",
            "ContentFolderView.xaml.cs");
        var hierarchySource = ReadRepositoryFile(
            "Editor",
            "Views",
            "HierarchyView.xaml.cs");
        var prefabDrop = Slice(
            contentSource,
            "if (command is CreatePrefabAssetCommand)",
            "var context = contextProvider.GetCurrentContext");
        var focus = Slice(
            hierarchySource,
            "async Task FocusGameObjectAsync(",
            "async Task BreakPrefabLinkAsync(");

        AssertInOrder(
            prefabDrop,
            "CommitPendingChangesAsync()",
            "EditorDragDrop.TryCreateContentDropCommand(");
        AssertInOrder(
            focus,
            "CommitPendingChangesAsync()",
            "gameObjectsById.TryGetValue(",
            "new FocusEditorCameraCommand(");
    }

    static void AssertCommitClearsDirtyBeforeDispatch(string source)
    {
        var commit = Slice(
            source,
            "public async Task<bool> CommitInspectorChangesAsync(",
            "public bool HasPendingInspectorChanges");

        AssertInOrder(commit, "IsDirty = false;", "dispatcher.DispatchAsync(");
        Assert.Contains("catch", commit, StringComparison.Ordinal);
        Assert.True(
            CountOccurrences(commit, "IsDirty = true;") >= 2,
            "A failed or throwing commit must restore the pending edit state.");
    }

    static void AssertCommitSkipsUnchangedYaml(string source)
    {
        var commit = Slice(
            source,
            "public async Task<bool> CommitInspectorChangesAsync(",
            "public bool HasPendingInspectorChanges");

        AssertInOrder(
            commit,
            "var previousYaml = _lastCommittedYaml",
            "string.Equals(previousYaml",
            "IsDirty = false;",
            "return false;",
            "dispatcher.DispatchAsync(");
    }

    static string Slice(string source, string startMarker, string endMarker)
    {
        var start = source.IndexOf(startMarker, StringComparison.Ordinal);
        Assert.True(start >= 0, $"Missing source marker: {startMarker}");

        var end = source.IndexOf(endMarker, start + startMarker.Length, StringComparison.Ordinal);
        Assert.True(end >= 0, $"Missing source marker: {endMarker}");
        return source[start..end];
    }

    static void AssertInOrder(string source, params string[] markers)
    {
        var previous = -1;
        foreach (var marker in markers)
        {
            var current = source.IndexOf(marker, previous + 1, StringComparison.Ordinal);
            Assert.True(current >= 0, $"Missing or out-of-order source marker: {marker}");
            previous = current;
        }
    }

    static int CountOccurrences(string source, string value)
    {
        var count = 0;
        var offset = 0;
        while ((offset = source.IndexOf(value, offset, StringComparison.Ordinal)) >= 0)
        {
            count++;
            offset += value.Length;
        }

        return count;
    }

    static string ReadRepositoryFile(params string[] relativePath)
    {
        var current = new DirectoryInfo(AppContext.BaseDirectory);
        while (current is not null)
        {
            var candidate = Path.Combine(relativePath.Prepend(current.FullName).ToArray());
            if (File.Exists(candidate))
                return File.ReadAllText(candidate).ReplaceLineEndings("\n");

            current = current.Parent;
        }

        throw new FileNotFoundException($"Could not find repository file: {Path.Combine(relativePath)}");
    }
}
