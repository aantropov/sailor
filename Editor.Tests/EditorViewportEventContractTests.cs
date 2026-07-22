using SailorEditor.Scene;

namespace Editor.Tests;

public sealed class EditorViewportEventContractTests
{
    [Fact]
    public void SelectionEvent_ParsesSelectedAndClearedSelection()
    {
        Assert.True(EditorViewportEventContract.TryParse(
            "kind: selection\nrevision: 17\nmanagedMutationRevision: 5\nselectedInstanceId: go-42\n",
            out var selected,
            out var selectedError), selectedError);
        var selection = Assert.IsType<EditorViewportSelectionEvent>(selected);
        Assert.Equal(17UL, selection.Revision);
        Assert.Equal(5UL, selection.ManagedMutationRevision);
        Assert.Equal("go-42", selection.SelectedInstanceId);

        Assert.True(EditorViewportEventContract.TryParse(
            "kind: selection\nrevision: 18\nmanagedMutationRevision: 5\nselectedInstanceId: ''\n",
            out var cleared,
            out var clearedError), clearedError);
        Assert.Equal(string.Empty, Assert.IsType<EditorViewportSelectionEvent>(cleared).SelectedInstanceId);
    }

    [Fact]
    public void TransformEvent_ParsesTypedOperationSpaceAndNumericTransform()
    {
        const string yaml = """
            kind: transform
            revision: 21
            managedMutationRevision: 8
            instanceId: go-42
            operation: Rotate
            space: Local
            beforePosition: [0, 0, 0, 1]
            beforeRotation: [0, 0, 0, 1]
            beforeScale: [1, 1, 1, 0]
            afterPosition: [1, 2.5, -3, 1]
            afterRotation: [0, 0.70710677, 0, 0.70710677]
            afterScale: [1, 2, 3, 0]
            """;

        Assert.True(EditorViewportEventContract.TryParse(yaml, out var parsed, out var error), error);
        var transform = Assert.IsType<EditorViewportTransformEvent>(parsed);
        Assert.Equal(21UL, transform.Revision);
        Assert.Equal(8UL, transform.ManagedMutationRevision);
        Assert.Equal("go-42", transform.InstanceId);
        Assert.Equal(EditorViewportTransformOperation.Rotate, transform.Operation);
        Assert.Equal(EditorViewportTransformSpace.Local, transform.Space);
        Assert.Equal(new EditorViewportVector4(0, 0, 0, 1), transform.BeforePosition);
        Assert.Equal(new EditorViewportVector4(1, 2.5f, -3, 1), transform.AfterPosition);
        Assert.Equal(new EditorViewportVector4(1, 2, 3, 0), transform.AfterScale);
    }

    [Theory]
    [InlineData("kind: unknown\nrevision: 1\nmanagedMutationRevision: 0\n", "Unsupported viewport event kind")]
    [InlineData("kind: selection\nrevision: 1\nmanagedMutationRevision: 0\nselectedInstanceId: go\nunexpected: true\n", "unexpected field")]
    [InlineData("kind: transform\nrevision: 2\nmanagedMutationRevision: 0\ninstanceId: ''\noperation: Translate\nspace: World\nbeforePosition: [0, 0, 0, 1]\nbeforeRotation: [0, 0, 0, 1]\nbeforeScale: [1, 1, 1, 0]\nafterPosition: [1, 0, 0, 1]\nafterRotation: [0, 0, 0, 1]\nafterScale: [1, 1, 1, 0]\n", "instanceId")]
    [InlineData("kind: transform\nrevision: 2\nmanagedMutationRevision: 0\ninstanceId: go\noperation: translate\nspace: World\nbeforePosition: [0, 0, 0, 1]\nbeforeRotation: [0, 0, 0, 1]\nbeforeScale: [1, 1, 1, 0]\nafterPosition: [1, 0, 0, 1]\nafterRotation: [0, 0, 0, 1]\nafterScale: [1, 1, 1, 0]\n", "unsupported value")]
    [InlineData("kind: transform\nrevision: 2\nmanagedMutationRevision: 0\ninstanceId: go\noperation: Scale\nspace: World\nbeforePosition: [0, 0, 0]\nbeforeRotation: [0, 0, 0, 1]\nbeforeScale: [1, 1, 1, 0]\nafterPosition: [1, 0, 0, 1]\nafterRotation: [0, 0, 0, 1]\nafterScale: [1, 1, 1, 0]\n", "exactly four")]
    [InlineData("kind: transform\nrevision: 2\nmanagedMutationRevision: 0\ninstanceId: go\noperation: Scale\nspace: World\nbeforePosition: [.inf, 0, 0, 1]\nbeforeRotation: [0, 0, 0, 1]\nbeforeScale: [1, 1, 1, 0]\nafterPosition: [1, 0, 0, 1]\nafterRotation: [0, 0, 0, 1]\nafterScale: [1, 1, 1, 0]\n", "invalid number")]
    public void InvalidPayload_IsRejected(string yaml, string expectedError)
    {
        Assert.False(EditorViewportEventContract.TryParse(yaml, out var parsed, out var error));
        Assert.Null(parsed);
        Assert.Contains(expectedError, error, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void RevisionGate_RejectsDuplicateAndStaleEventsAcrossKinds()
    {
        var gate = new EditorViewportEventRevisionGate();

        Assert.True(gate.TryAccept(new EditorViewportSelectionEvent(7, 0, "go-1")));
        Assert.False(gate.TryAccept(Transform(7)));
        Assert.False(gate.TryAccept(Transform(6)));
        Assert.True(gate.TryAccept(Transform(8)));
        Assert.Equal(8UL, gate.LastAcceptedRevision);

        gate.Reset();
        Assert.Null(gate.LastAcceptedRevision);
        Assert.True(gate.TryAccept(new EditorViewportSelectionEvent(0, 0, string.Empty)));
    }

    [Fact]
    public void ManagedMutationOrder_RejectsDelayedViewportEventAfterNewerManagedEdit()
    {
        const ulong eventManagedMutationRevision = 12;

        Assert.True(EditorViewportMutationOrder.IsCurrent(eventManagedMutationRevision, 12));
        Assert.False(EditorViewportMutationOrder.IsCurrent(eventManagedMutationRevision, 13));
    }

    [Fact]
    public void SceneEvents_CheckNativeMutationFence_AndInvalidateQueuedSnapshots()
    {
        var sceneSource = ReadRepositoryFile("Editor", "Views", "SceneView.xaml.cs");
        var engineSource = ReadRepositoryFile("Editor", "Services", "EngineService.cs");
        var targetSource = ReadRepositoryFile("Editor", "Commands", "EditorViewportTransformTarget.cs");

        var selectionApply = Slice(sceneSource, "async Task ApplyViewportSelectionAsync", "async Task ApplyViewportTransformAsync");
        var transformApply = Slice(sceneSource, "async Task ApplyViewportTransformAsync", "ActionContext CreateViewportActionContext");
        Assert.Equal(2, CountOccurrences(selectionApply, "IsEditorViewportSelectionEventCurrent(viewportEvent.ManagedMutationRevision)"));
        Assert.Equal(2, CountOccurrences(transformApply, "IsEditorViewportTransformEventCurrent("));
        AssertFenceIsRecheckedAfterInspectorFlush(
            selectionApply,
            "IsEditorViewportSelectionEventCurrent(viewportEvent.ManagedMutationRevision)");
        AssertFenceIsRecheckedAfterInspectorFlush(
            transformApply,
            "IsEditorViewportTransformEventCurrent(");
        Assert.Contains("viewportEvent.InstanceId", transformApply, StringComparison.Ordinal);
        Assert.Contains("GetEditorManagedMutationRevision(uint kind, string strInstanceId)", engineSource, StringComparison.Ordinal);
        Assert.Contains("EngineAppInterop.GetEditorManagedMutationRevision((uint)kind, instanceId)", engineSource, StringComparison.Ordinal);
        Assert.Contains("Selection = 1", engineSource, StringComparison.Ordinal);
        Assert.Contains("ObjectTransform = 2", engineSource, StringComparison.Ordinal);

        var invalidate = targetSource.IndexOf("engineService.InvalidateQueuedWorldSnapshots()", StringComparison.Ordinal);
        var applyLocal = targetSource.IndexOf("worldService.ApplyGameObjectYamlLocal", invalidate, StringComparison.Ordinal);
        Assert.True(invalidate >= 0);
        Assert.True(applyLocal > invalidate);
    }

    [Fact]
    public void PrimaryPointerPress_FlushesInspectorBeforeNativeGestureStarts()
    {
        var source = ReadRepositoryFile("Editor", "Views", "SceneView.xaml.cs");
        var inputHandler = Slice(source, "nativeViewportHost.InputReceived += input =>", "NativeViewportContainer.Content = nativeViewportHost");

        AssertInOrder(
            inputHandler,
            "input.Button == 0",
            "inspectorPendingEditCoordinator.CommitPendingChanges()",
            "isInputCaptured = true",
            "viewportAdapter.SendInput(");
    }

    [Fact]
    public void NativeSelectionProjection_IsLocalOnlySoOneBatchCanApplyAThenB()
    {
        var sceneSource = ReadRepositoryFile("Editor", "Views", "SceneView.xaml.cs");
        var commandsSource = ReadRepositoryFile("Editor", "Commands", "EditorWorldCommands.cs");
        var selectionSource = ReadRepositoryFile("Editor", "Services", "SelectionService.cs");

        var selectionApply = Slice(sceneSource, "async Task ApplyViewportSelectionAsync", "async Task ApplyViewportTransformAsync");
        Assert.Contains("new ApplyRuntimeSelectionCommand(selectedId)", selectionApply, StringComparison.Ordinal);
        Assert.DoesNotContain("new SelectObjectCommand", selectionApply, StringComparison.Ordinal);

        var command = Slice(commandsSource, "public sealed class ApplyRuntimeSelectionCommand", "public sealed class UpdateGameObjectCommand");
        Assert.Contains("ApplyRuntimeSelection(_instanceId)", command, StringComparison.Ordinal);

        var localProjection = Slice(selectionSource, "public void ApplyRuntimeSelection", "public void ResetForDocumentChange");
        var suppress = localProjection.IndexOf("Interlocked.Increment(ref suppressRuntimeSelectionSync)", StringComparison.Ordinal);
        var select = localProjection.IndexOf("SelectInstance(instanceId)", StringComparison.Ordinal);
        var release = localProjection.IndexOf("Interlocked.Decrement(ref suppressRuntimeSelectionSync)", StringComparison.Ordinal);
        Assert.True(suppress >= 0);
        Assert.True(select > suppress);
        Assert.True(release > select);
        Assert.DoesNotContain("workspaceResetInProgress", localProjection, StringComparison.Ordinal);
        Assert.Contains(
            "IsWorkspaceResetInProgress => Volatile.Read(ref workspaceResetInProgress) != 0",
            selectionSource,
            StringComparison.Ordinal);

        const ulong currentSelectionRevision = 4;
        EditorViewportSelectionEvent[] batch =
        [
            new(17, currentSelectionRevision, "go-a"),
            new(18, currentSelectionRevision, "go-b"),
        ];
        var appliedSelectionIds = batch
            .Where(viewportEvent => EditorViewportMutationOrder.IsCurrent(
                viewportEvent.ManagedMutationRevision,
                currentSelectionRevision))
            .Select(viewportEvent => viewportEvent.SelectedInstanceId)
            .ToArray();
        Assert.Equal(["go-a", "go-b"], appliedSelectionIds);
    }

    [Fact]
    public void EpochGate_RejectsBatchesCapturedBeforeDocumentChange()
    {
        var gate = new EditorViewportEventEpochGate();
        var previousDocument = gate.Current;

        Assert.True(gate.IsCurrent(previousDocument));
        Assert.Equal(previousDocument + 1, gate.Advance());
        Assert.False(gate.IsCurrent(previousDocument));
        Assert.True(gate.IsCurrent(gate.Current));
    }

    [Fact]
    public void EnginePolling_UsesUtf8OwnershipAndDocumentEpochGuards()
    {
        var source = ReadRepositoryFile("Editor", "Services", "EngineService.cs");

        var pull = Slice(source, "IReadOnlyList<EditorViewportEvent> PullEditorViewportEvents", "string SerializeWorld");
        Assert.Contains("Marshal.PtrToStringUTF8", pull, StringComparison.Ordinal);
        Assert.Contains("finally", pull, StringComparison.Ordinal);
        Assert.Contains("EngineAppInterop.FreeInteropString(eventPtr)", pull, StringComparison.Ordinal);
        Assert.Contains("eventEpoch = editorViewportEventEpoch.Current", pull, StringComparison.Ordinal);
        Assert.Contains("editorViewportEventEpoch.IsCurrent(eventEpoch)", pull, StringComparison.Ordinal);

        var publish = Slice(source, "void PublishEditorViewportEvents", "bool InvokeRunningInterop");
        Assert.Contains("IsGenerationActive(generation)", publish, StringComparison.Ordinal);
        Assert.Contains("editorViewportEventEpoch.IsCurrent(eventEpoch)", publish, StringComparison.Ordinal);

        var load = Slice(source, "public bool LoadWorld", "public bool CreateWorld");
        var create = Slice(source, "public bool CreateWorld", "public string SerializeCurrentWorld");
        Assert.Contains("editorViewportEventEpoch.Advance()", load, StringComparison.Ordinal);
        Assert.Contains("editorViewportEventEpoch.Advance()", create, StringComparison.Ordinal);
    }

    [Fact]
    public void SceneTransform_UsesNativeBeforeAfterWithOneStructuralSnapshot()
    {
        var source = ReadRepositoryFile("Editor", "Views", "SceneView.xaml.cs");
        var apply = Slice(source, "async Task ApplyViewportTransformAsync", "ActionContext CreateViewportActionContext");

        Assert.Equal(1, CountOccurrences(apply, "CaptureGameObjectStructure(gameObject)"));
        Assert.Contains("viewportEvent.BeforePosition", apply, StringComparison.Ordinal);
        Assert.Contains("viewportEvent.BeforeRotation", apply, StringComparison.Ordinal);
        Assert.Contains("viewportEvent.BeforeScale", apply, StringComparison.Ordinal);
        Assert.Contains("viewportEvent.AfterPosition", apply, StringComparison.Ordinal);
        Assert.Contains("viewportEvent.AfterRotation", apply, StringComparison.Ordinal);
        Assert.Contains("viewportEvent.AfterScale", apply, StringComparison.Ordinal);
        Assert.Contains("Name = structure.Name", source, StringComparison.Ordinal);
        Assert.Contains("InstanceId = new InstanceId(structure.InstanceId)", source, StringComparison.Ordinal);
        Assert.Contains("ParentIndex = structure.ParentIndex", source, StringComparison.Ordinal);
        Assert.Contains("ComponentIndices = new List<int>(structure.ComponentIndices)", source, StringComparison.Ordinal);
    }

    static EditorViewportTransformEvent Transform(ulong revision) => new(
        revision,
        0,
        "go-1",
        EditorViewportTransformOperation.Translate,
        EditorViewportTransformSpace.World,
        new EditorViewportVector4(0, 0, 0, 1),
        new EditorViewportVector4(0, 0, 0, 1),
        new EditorViewportVector4(1, 1, 1, 0),
        new EditorViewportVector4(1, 0, 0, 1),
        new EditorViewportVector4(0, 0, 0, 1),
        new EditorViewportVector4(1, 1, 1, 0));

    static void AssertFenceIsRecheckedAfterInspectorFlush(string source, string fenceMarker)
    {
        var firstFence = source.IndexOf(fenceMarker, StringComparison.Ordinal);
        var flush = source.IndexOf("inspectorPendingEditCoordinator.CommitPendingChanges()", firstFence, StringComparison.Ordinal);
        var secondFence = source.IndexOf(fenceMarker, firstFence + fenceMarker.Length, StringComparison.Ordinal);

        Assert.True(firstFence >= 0);
        Assert.True(flush > firstFence);
        Assert.True(secondFence > flush);
    }

    static void AssertInOrder(string source, params string[] values)
    {
        var offset = 0;
        foreach (var value in values)
        {
            var index = source.IndexOf(value, offset, StringComparison.Ordinal);
            Assert.True(index >= 0, $"Missing or out-of-order source marker: {value}");
            offset = index + value.Length;
        }
    }

    static string Slice(string source, string startMarker, string endMarker)
    {
        var start = source.IndexOf(startMarker, StringComparison.Ordinal);
        var end = source.IndexOf(endMarker, start + startMarker.Length, StringComparison.Ordinal);
        Assert.True(start >= 0, $"Missing source marker: {startMarker}");
        Assert.True(end > start, $"Missing source marker after {startMarker}: {endMarker}");
        return source[start..end];
    }

    static int CountOccurrences(string source, string value)
    {
        var count = 0;
        var offset = 0;
        while ((offset = source.IndexOf(value, offset, StringComparison.Ordinal)) >= 0)
        {
            ++count;
            offset += value.Length;
        }

        return count;
    }

    static string ReadRepositoryFile(params string[] relativePath)
    {
        var current = new DirectoryInfo(AppContext.BaseDirectory);
        while (current is not null)
        {
            if (Directory.Exists(Path.Combine(current.FullName, "Editor")) &&
                Directory.Exists(Path.Combine(current.FullName, "Runtime")))
            {
                return File.ReadAllText(Path.Combine([current.FullName, .. relativePath]));
            }

            current = current.Parent;
        }

        throw new DirectoryNotFoundException("Could not find the Sailor repository root.");
    }
}
