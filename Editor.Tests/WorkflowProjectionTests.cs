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
        AssertInOrder(
            mutationMethods,
            "ShowAsync(",
            "WorldService>().AddComponent(this, componentTypeName)",
            "public void ClearComponentsFromInspector()",
            "WorldService>().RemoveComponent(component)");
    }

    [Fact]
    public void DestroyGameObjectUndo_UsesAnInMemoryPrefabSnapshot()
    {
        var source = ReadRepositoryFile("Editor", "Commands", "EditorWorldCommands.cs");
        var destroyCommand = Slice(
            source,
            "public sealed class DestroyGameObjectCommand",
            "public sealed class ReparentGameObjectCommand");

        Assert.DoesNotContain("CreatePrefabAsset", destroyCommand, StringComparison.Ordinal);
        Assert.Contains("CreatePrefabFromSubHierarchy", destroyCommand, StringComparison.Ordinal);
        Assert.Contains("EditorYaml.SerializePrefab", destroyCommand, StringComparison.Ordinal);
        Assert.Contains("InstantiatePrefabFromYaml", destroyCommand, StringComparison.Ordinal);
        Assert.Contains("CommandResult.Failure(\"Restore deleted hierarchy failed\")", destroyCommand, StringComparison.Ordinal);
        Assert.Contains("CommandResult.Failure(\"Restored hierarchy root was not found\")", destroyCommand, StringComparison.Ordinal);
    }

    [Fact]
    public void PrefabSnapshotInterop_MarshalsYamlAsUtf8()
    {
        var source = ReadRepositoryFile("Editor", "Services", "EngineService.cs");

        Assert.Contains(
            "InstantiatePrefabFromYaml([MarshalAs(UnmanagedType.LPUTF8Str)] string strPrefabYaml, [MarshalAs(UnmanagedType.LPUTF8Str)] string strParentInstanceId)",
            source,
            StringComparison.Ordinal);
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
            "public void RefreshCurrentWorld()",
            "bool InvokeCreationInterop");

        AssertInOrder(
            createCommand,
            "CreateGameObject(parentId, _createdId, out var createdId)",
            "_createdId = createdId;",
            "TryGetGameObject(createdId",
            "DestroyObject(createdId)");
        AssertInOrder(
            addCommand,
            "AddComponent(_ownerId, componentTypeName, _componentId, out var createdId)",
            "_componentId = createdId;",
            "TryGetComponent(createdId",
            "RemoveComponent(createdId)");
        AssertInOrder(
            removeCommand,
            "AddComponent(_ownerId, _componentTypeName, _originalInstanceId, out var restoredInstanceId)",
            "CommitChanges(restored.InstanceId, _beforeYaml)",
            "ApplyComponentYamlLocal(restored.InstanceId, _beforeYaml)",
            "_activeInstanceId = restoredInstanceId;");
        Assert.DoesNotContain("QueueWorldUpdate", refresh, StringComparison.Ordinal);
        Assert.Contains("MainThread.InvokeOnMainThreadAsync", refresh, StringComparison.Ordinal);
        Assert.Contains("GetAwaiter().GetResult()", refresh, StringComparison.Ordinal);
    }

    static void AssertCommitClearsDirtyBeforeDispatch(string source)
    {
        var commit = Slice(
            source,
            "public bool CommitInspectorChanges()",
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
            "public bool CommitInspectorChanges()",
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
                return File.ReadAllText(candidate);

            current = current.Parent;
        }

        throw new FileNotFoundException($"Could not find repository file: {Path.Combine(relativePath)}");
    }
}
