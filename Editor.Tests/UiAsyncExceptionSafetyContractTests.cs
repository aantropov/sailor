namespace Editor.Tests;

public sealed class UiAsyncExceptionSafetyContractTests
{
    [Fact]
    public void InspectorFireAndForgetTasks_AreObservedAndSharedWaitHonorsCancellation()
    {
        var templatesSource = ReadRepositoryFile(
            "Editor",
            "Utility",
            "Templates.cs");
        var inspectorSource = ReadRepositoryFile(
            "Editor",
            "Views",
            "InspectorView.xaml.cs");

        Assert.DoesNotContain(
            "static async void CommitInspectorBindingContext",
            templatesSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "_ = CommitInspectorBindingContextAsync(editable);",
            templatesSource,
            StringComparison.Ordinal);
        AssertInOrder(
            Slice(
                templatesSource,
                "static async Task CommitInspectorBindingContextAsync(",
                "static void ScheduleInspectorCommit("),
            "try",
            "await editable.CommitInspectorChangesAsync();",
            "catch (Exception ex)",
            "Console.Error.WriteLine");

        Assert.DoesNotContain(
            "async void",
            inspectorSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "_ = CommitPendingInspectorChangesOnUnloadAsync();",
            inspectorSource,
            StringComparison.Ordinal);
        AssertInOrder(
            Slice(
                inspectorSource,
                "void UnsubscribeFromLifecycle()",
                "async Task CommitPendingInspectorChangesOnUnloadAsync()"),
            "CommitPendingChangesRequested -= CommitPendingInspectorChanges",
            "inspectorProjection.PropertyChanged -= OnProjectionChanged",
            "lifecycleSubscribed = false",
            "_ = CommitPendingInspectorChangesOnUnloadAsync();");
        Assert.Contains(
            "_ = RefreshInspectorLoopSafelyAsync();",
            inspectorSource,
            StringComparison.Ordinal);
        AssertInOrder(
            Slice(
                inspectorSource,
                "async Task RefreshInspectorLoopSafelyAsync()",
                "async Task RefreshInspectorAsync()"),
            "try",
            "while (inspectorRefreshRequested)",
            "inspectorRefreshRequested = false;",
            "await RefreshInspectorAsync();",
            "catch (Exception ex)",
            "Console.Error.WriteLine",
            "finally",
            "isRefreshingInspector = false;",
            "if (inspectorRefreshRequested)",
            "QueueInspectorRefresh();");
        Assert.Contains(
            "await commitTask.WaitAsync(cancellationToken)",
            inspectorSource,
            StringComparison.Ordinal);
    }

    [Fact]
    public void HierarchyAndComponentContextCommands_ObserveAsyncFailures()
    {
        var hierarchySource = ReadRepositoryFile(
            "Editor",
            "Views",
            "HierarchyView.xaml.cs");
        var componentSource = ReadRepositoryFile(
            "Editor",
            "Views",
            "InspectorView",
            "ComponentTemplate.cs");
        var hierarchyMenu = Slice(
            hierarchySource,
            "MenuFlyout CreateHierarchyContextFlyout(",
            "static Command CreateContextMenuCommand(");
        var componentMenu = Slice(
            componentSource,
            "var contextItems = new[]",
            "var flyout = contextMenuService.CreateFlyout(");

        Assert.DoesNotMatch(
            @"new\s+Command\s*\(\s*async",
            hierarchyMenu);
        Assert.DoesNotMatch(
            @"new\s+Command\s*\(\s*async",
            componentMenu);
        Assert.Contains(
            "Command = CreateContextMenuCommand(",
            hierarchyMenu,
            StringComparison.Ordinal);
        Assert.Contains(
            "Command = CreateContextMenuCommand(",
            componentMenu,
            StringComparison.Ordinal);
        AssertAsyncActionHelperReportsFailure(hierarchySource);
        AssertAsyncActionHelperReportsFailure(componentSource);
    }

    [Fact]
    public void ContentHandlersAndContextCommands_ObserveAsyncFailures()
    {
        var contentSource = ReadRepositoryFile(
            "Editor",
            "Views",
            "ContentFolderView.xaml.cs");
        var helper = Slice(
            contentSource,
            "static async Task ExecuteContentUiActionAsync(",
            "void EnsureFolderVisible(");

        Assert.DoesNotContain(
            "Drop += async",
            contentSource,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "async void OnContentSelectionChanged",
            contentSource,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "Tapped += async",
            contentSource,
            StringComparison.Ordinal);
        var contextMenu = Slice(
            contentSource,
            "void ShowContextMenu(object model)",
            "static Command CreateContentContextMenuCommand(");
        Assert.DoesNotMatch(
            @"new\s+Command\s*\(\s*async",
            contextMenu);
        Assert.True(
            CountOccurrences(
                contextMenu,
                "Command = CreateContentContextMenuCommand(") >= 7);
        Assert.True(
            CountOccurrences(
                contentSource,
                "RunContentUiAction(") >= 4);
        AssertInOrder(
            helper,
            "try",
            "await action();",
            "catch (Exception exception)",
            "Console.Error.WriteLine",
            "DisplayAlert(",
            "catch (Exception reportingException)");
    }

    [Fact]
    public void ToolbarAndNativeToolbarHandlers_ObserveAsyncFailures()
    {
        var toolbarSource = ReadRepositoryFile(
            "Editor",
            "Views",
            "ToolbarView.xaml.cs");
        var nativeToolbarSource = ReadRepositoryFile(
            "Editor",
            "Platforms",
            "MacCatalyst",
            "MacCatalystWindowChrome.cs");

        Assert.DoesNotContain(
            "async void",
            toolbarSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "_ = RunToolbarActionSafelyAsync(",
            toolbarSource,
            StringComparison.Ordinal);
        AssertInOrder(
            Slice(
                toolbarSource,
                "async Task RunToolbarActionSafelyAsync(",
                "\n    }\n}"),
            "try",
            "await action();",
            "catch (Exception ex)",
            "Console.Error.WriteLine",
            "SetStatus(");

        Assert.DoesNotContain(
            "public async void Invoke(",
            nativeToolbarSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "public void Invoke(NSObject sender)",
            nativeToolbarSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "_ = InvokeAsync();",
            nativeToolbarSource,
            StringComparison.Ordinal);
        AssertInOrder(
            Slice(
                nativeToolbarSource,
                "async Task InvokeAsync()",
                "\n            }\n        }\n    }\n}"),
            "try",
            "InvokeOnMainThreadAsync(action)",
            "catch (Exception ex)",
            "Console.Error.WriteLine",
            "GetService<EditorShellHost>()",
            "SetStatus(");
    }

    static void AssertAsyncActionHelperReportsFailure(string source)
    {
        Assert.Contains(
            "_ = ExecuteContextMenuActionAsync(",
            source,
            StringComparison.Ordinal);
        AssertInOrder(
            Slice(
                source,
                "static async Task ExecuteContextMenuActionAsync(",
                "Failed to publish"),
            "try",
            "await action();",
            "catch (Exception ex)",
            "Console.Error.WriteLine",
            "GetService<EditorShellHost>()",
            "SetStatus(");
    }

    static string Slice(
        string source,
        string startMarker,
        string endMarker)
    {
        var start = source.IndexOf(
            startMarker,
            StringComparison.Ordinal);
        Assert.True(
            start >= 0,
            $"Missing source marker: {startMarker}");

        var end = source.IndexOf(
            endMarker,
            start + startMarker.Length,
            StringComparison.Ordinal);
        Assert.True(
            end > start,
            $"Missing source marker after start: {endMarker}");
        return source[start..end];
    }

    static void AssertInOrder(
        string source,
        params string[] markers)
    {
        var previous = -1;
        foreach (var marker in markers)
        {
            var current = source.IndexOf(
                marker,
                previous + 1,
                StringComparison.Ordinal);
            Assert.True(
                current >= 0,
                $"Missing or out-of-order source marker: {marker}");
            previous = current;
        }
    }

    static int CountOccurrences(
        string source,
        string value)
    {
        var count = 0;
        var offset = 0;
        while ((offset = source.IndexOf(
                   value,
                   offset,
                   StringComparison.Ordinal)) >= 0)
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
            var candidate = Path.Combine(
                relativePath.Prepend(current.FullName).ToArray());
            if (File.Exists(candidate))
            {
                return File.ReadAllText(candidate).ReplaceLineEndings("\n");
            }

            current = current.Parent;
        }

        throw new FileNotFoundException(
            $"Could not find repository file: {Path.Combine(relativePath)}");
    }
}
