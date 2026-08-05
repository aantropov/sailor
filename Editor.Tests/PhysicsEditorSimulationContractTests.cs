namespace SailorEditor.Tests;

public sealed class PhysicsEditorSimulationContractTests
{
    [Fact]
    public void NativeSimulation_SnapshotsBeforeStartAndReplacesWorldOnStop()
    {
        var source = ReadRepositoryFile(
            "Runtime",
            "Submodules",
            "Editor.cpp");

        Assert.Contains("const YAML::Node snapshot = SerializeWorld();", source, StringComparison.Ordinal);
        Assert.Contains("External::TryDumpYaml(", source, StringComparison.Ordinal);
        Assert.Contains("m_world->SetPhysicsSimulationEnabled(true);", source, StringComparison.Ordinal);
        Assert.Contains("External::TryLoadYaml(", source, StringComparison.Ordinal);
        Assert.Contains("engineLoop->InstantiateWorld(", source, StringComparison.Ordinal);
        Assert.Contains("SetWorld(restoredWorld.GetRawPtr());", source, StringComparison.Ordinal);
        Assert.Contains("engineLoop->ExitWorld(oldWorld);", source, StringComparison.Ordinal);
    }

    [Fact]
    public void Toolbar_UsesNativeStateAndRefreshesAuthoritativeProjection()
    {
        var actions = ReadRepositoryFile(
            "Editor",
            "Services",
            "EditorToolbarActions.cs");
        var worldService = ReadRepositoryFile(
            "Editor",
            "Services",
            "WorldService.cs");

        Assert.Contains("GetEditorSimulationStateAsync", actions, StringComparison.Ordinal);
        Assert.Contains("CommitInspectorChangesAsync", actions, StringComparison.Ordinal);
        Assert.Contains("SetEditorSimulationAsync", actions, StringComparison.Ordinal);
        Assert.Contains("RefreshCurrentWorldAuthoritativelyAsync", actions, StringComparison.Ordinal);
        Assert.Contains("Stop simulation before saving the scene.", worldService, StringComparison.Ordinal);
    }

    [Fact]
    public void Toolbar_SimulationIconTracksNativeState()
    {
        var toolbar = ReadRepositoryFile(
            "Editor",
            "Views",
            "ToolbarView.xaml.cs");
        var nativeToolbar = ReadRepositoryFile(
            "Editor",
            "Platforms",
            "MacCatalyst",
            "MacCatalystWindowChrome.cs");

        Assert.Contains("control_stop_square.png", toolbar, StringComparison.Ordinal);
        Assert.Contains("control.png", toolbar, StringComparison.Ordinal);
        Assert.Contains("stop.circle.fill", nativeToolbar, StringComparison.Ordinal);
        Assert.Contains("play.circle", nativeToolbar, StringComparison.Ordinal);
        Assert.Contains("BeginInvokeOnMainThread", nativeToolbar, StringComparison.Ordinal);
    }

    [Fact]
    public void PhysicsStep_UsesDedicatedQueueAndJoltJobsUseWorkers()
    {
        var physicsEcs = ReadRepositoryFile(
            "Runtime",
            "ECS",
            "PhysicsECS.cpp");
        var jobSystem = ReadRepositoryFile(
            "Runtime",
            "Physics",
            "JoltJobSystem.cpp");
        var scheduler = ReadRepositoryFile(
            "Runtime",
            "Tasks",
            "Scheduler.cpp");

        Assert.Contains("EThreadType::Physics", physicsEcs, StringComparison.Ordinal);
        Assert.Contains("physicsTask->Wait();", physicsEcs, StringComparison.Ordinal);
        Assert.Contains("EThreadType::Worker", jobSystem, StringComparison.Ordinal);
        Assert.Contains("JobSystemWithBarrier::WaitForJobs", jobSystem, StringComparison.Ordinal);
        Assert.Contains("m_numQueuedTasks.wait", jobSystem, StringComparison.Ordinal);
        Assert.Contains("\"Physics Thread\"", scheduler, StringComparison.Ordinal);
    }

    static string ReadRepositoryFile(params string[] relativePath)
    {
        var path = Path.Combine(
            FindRepositoryRoot(),
            Path.Combine(relativePath));
        return File.ReadAllText(path);
    }

    static string FindRepositoryRoot()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        while (directory is not null)
        {
            if (File.Exists(Path.Combine(directory.FullName, "Protocol", "editor_engine.proto")) &&
                Directory.Exists(Path.Combine(directory.FullName, "Runtime")))
            {
                return directory.FullName;
            }

            directory = directory.Parent;
        }

        throw new DirectoryNotFoundException("Could not find the Sailor repository root.");
    }
}
