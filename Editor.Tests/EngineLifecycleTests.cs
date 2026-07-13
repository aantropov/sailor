namespace Editor.Tests;

public sealed class EngineLifecycleTests
{
    [Fact]
    public void ManagedLifecycle_ExposesExplicitAwaitableOwnershipContract()
    {
        var source = ReadRepositoryFile("Editor", "Services", "EngineService.cs");

        Assert.Contains("public enum EngineLifecycleState", source, StringComparison.Ordinal);
        Assert.Contains("public Task StartAsync(EngineLaunchContext launchContext, CancellationToken cancellationToken = default)", source, StringComparison.Ordinal);
        Assert.Contains("public async Task<int> StopAsync(CancellationToken cancellationToken = default)", source, StringComparison.Ordinal);
        Assert.Contains("public void ResetForWorkspaceChange()", source, StringComparison.Ordinal);
        Assert.Contains("await session.NativeRunTask.ConfigureAwait(false)", source, StringComparison.Ordinal);
        Assert.Contains("await Task.WhenAll(session.PollTasks).ConfigureAwait(false)", source, StringComparison.Ordinal);
        Assert.Contains("EngineAppInterop.Shutdown()", source, StringComparison.Ordinal);
        Assert.Contains("BuildInteropArgumentsAsync", source, StringComparison.Ordinal);
        Assert.Contains("MainThread.InvokeOnMainThreadAsync", source, StringComparison.Ordinal);
    }

    [Fact]
    public void DeferredPublications_AreRejectedAfterGenerationChanges()
    {
        var source = ReadRepositoryFile("Editor", "Services", "EngineService.cs");

        Assert.Contains("Interlocked.Increment(ref engineGeneration)", source, StringComparison.Ordinal);
        Assert.Contains("if (IsGenerationActive(generation, allowStarting: true))", source, StringComparison.Ordinal);
        Assert.Contains("message.Generation == generation", source, StringComparison.Ordinal);
        Assert.Contains(".Where(message => message.Generation == generation)", source, StringComparison.Ordinal);
        Assert.Contains("QueueWorldUpdate", source, StringComparison.Ordinal);
    }

    [Fact]
    public void NativeExitCode_IsAvailableOnBothExportSurfacesAndManagedInterop()
    {
        var managedSource = ReadRepositoryFile("Editor", "Services", "EngineService.cs");
        var windowsSource = ReadRepositoryFile("Lib", "DllMain.cpp");
        var unixSource = ReadRepositoryFile("Lib", "InteropExports.cpp");

        Assert.Contains("extern int GetExitCode()", managedSource, StringComparison.Ordinal);
        Assert.Contains("SAILOR_API int32_t GetExitCode()", windowsSource, StringComparison.Ordinal);
        Assert.Contains("return Sailor::App::GetExitCode();", windowsSource, StringComparison.Ordinal);
        Assert.Contains("SAILOR_API int32_t GetExitCode()", unixSource, StringComparison.Ordinal);
        Assert.Contains("return Sailor::App::GetExitCode();", unixSource, StringComparison.Ordinal);
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
