namespace Editor.Tests;

public sealed class WorkspaceIsolationRegressionTests
{
    [Fact]
    public void SelectionReset_CancelsPendingLoadsAndRejectsPriorEpochContinuations()
    {
        var source = ReadRepositoryFile("Editor", "Services", "SelectionService.cs");
        var selectObject = Slice(source, "public async void SelectObject", "public void ClearSelection");
        var begin = Slice(source, "public void BeginWorkspaceChange", "public void ResetForWorkspaceChange");
        var reset = Slice(source, "public void ResetForWorkspaceChange", "void ClearSelectionCore");
        var currentRequest = Slice(source, "bool IsCurrentRequest", "void UpdateSelection");

        Assert.Contains("var requestEpoch = WorkspaceEpoch;", selectObject, StringComparison.Ordinal);
        Assert.Contains("Interlocked.Exchange(ref pendingSelectionCancellation, requestCancellation)", selectObject, StringComparison.Ordinal);
        Assert.Contains("previousCancellation?.Cancel();", selectObject, StringComparison.Ordinal);
        Assert.Contains("WaitAsync(requestCancellation.Token)", selectObject, StringComparison.Ordinal);
        Assert.Contains("IsCurrentRequest(requestVersion, requestEpoch, requestCancellation.Token)", selectObject, StringComparison.Ordinal);

        AssertInOrder(
            begin,
            "Interlocked.Exchange(ref workspaceChangeInProgress, 1)",
            "Interlocked.Increment(ref workspaceEpoch)",
            "CancelPendingSelection()");
        AssertInOrder(
            reset,
            "BeginWorkspaceChange()",
            "ClearSelectionCore()");
        Assert.Contains("!cancellationToken.IsCancellationRequested", currentRequest, StringComparison.Ordinal);
        Assert.Contains("requestVersion == Volatile.Read(ref selectionRequestVersion)", currentRequest, StringComparison.Ordinal);
        Assert.Contains("requestEpoch == WorkspaceEpoch", currentRequest, StringComparison.Ordinal);
    }

    [Fact]
    public void WorldReset_DropsSameNameCachesAndEpochTagsDeferredUpdates()
    {
        var source = ReadRepositoryFile("Editor", "Services", "WorldService.cs");
        var subscribe = Slice(source, "void SubscribeToWorldUpdates", "public void ResetForWorkspaceChange");
        var reset = Slice(source, "public void ResetForWorkspaceChange", "static string GetWorldKey");
        var populate = Slice(source, "public bool TryPopulateWorld", "void RefreshSelection");

        Assert.Contains("var subscribedEpoch = WorkspaceEpoch;", subscribe, StringComparison.Ordinal);
        Assert.Contains("yaml => TryPopulateWorld(yaml, subscribedEpoch)", subscribe, StringComparison.Ordinal);
        AssertInOrder(
            reset,
            "Interlocked.Increment(ref workspaceEpoch)",
            "engineService.OnUpdateCurrentWorldAction -= worldUpdateHandler",
            "worldCaches.Clear()",
            "Current = new World()",
            "SubscribeToWorldUpdates()");

        Assert.True(
            CountOccurrences(populate, "expectedWorkspaceEpoch != WorkspaceEpoch") >= 2,
            "The callback epoch must be checked both before and after deserialization.");
        AssertInOrder(
            populate,
            "if (expectedWorkspaceEpoch != WorkspaceEpoch)",
            "var world = deserializer.Deserialize<World>(yaml)",
            "if (expectedWorkspaceEpoch != WorkspaceEpoch)",
            "worldCaches.TryGetValue(worldKey, out var cache)");
    }

    [Fact]
    public void InspectorRefresh_DiscardsPendingEditsDuringWorkspaceReset()
    {
        var source = ReadRepositoryFile("Editor", "Views", "InspectorView.xaml.cs");
        var refresh = Slice(source, "void RefreshInspector", "static bool AreEquivalentSelection");

        AssertInOrder(
            refresh,
            "if (!inspectorProjection.IsWorkspaceResetInProgress &&",
            "editable.HasPendingInspectorChanges",
            "editable.CommitInspectorChanges()");
    }

    [Fact]
    public void ActivationClear_RunsOnUiThreadInDependencySafeOrder()
    {
        var source = ReadRepositoryFile("Editor", "Services", "WorkspaceActivationOperations.cs");
        var clear = Slice(source, "public Task ClearAsync", "public async Task CommitAsync");

        Assert.Contains("return MainThread.InvokeOnMainThreadAsync(() =>", clear, StringComparison.Ordinal);
        Assert.Contains("commandHistory.ResetForWorkspaceChange();", clear, StringComparison.Ordinal);
        Assert.DoesNotContain("Reset(commandHistory.ResetForWorkspaceChange", clear, StringComparison.Ordinal);
        AssertInOrder(
            clear,
            "MainThread.InvokeOnMainThreadAsync",
            "commandHistory.ResetForWorkspaceChange",
            "selectionService.ResetForWorkspaceChange",
            "worldService.ResetForWorkspaceChange",
            "engineService.ResetForWorkspaceChange",
            "assetsService.ResetForWorkspaceChange",
            "projectContentStore.ResetForWorkspaceChange",
            "hierarchyProjection.ResetForWorkspaceChange",
            "inspectorProjection.ResetForWorkspaceChange");
    }

    [Fact]
    public void ActivationStop_AwaitsCommandDrainBeforeEngineTeardown()
    {
        var source = ReadRepositoryFile("Editor", "Services", "WorkspaceActivationOperations.cs");
        var stop = Slice(source, "public async Task StopAsync", "public Task ClearAsync");

        AssertInOrder(
            stop,
            "commandHistory.BeginWorkspaceChange",
            "selectionService.BeginWorkspaceChange",
            "await commandHistory.BeginWorkspaceChangeAsync(CancellationToken.None)",
            "engineService.StopAsync(cancellationToken)");
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
            {
                return File.ReadAllText(candidate);
            }

            current = current.Parent;
        }

        throw new FileNotFoundException($"Could not find repository file: {Path.Combine(relativePath)}");
    }
}
