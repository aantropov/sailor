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

    [Fact]
    public void AssetReload_IsQueuedToTheEngineThreadAndBoundToMacCommandR()
    {
        var managedSource = ReadRepositoryFile("Editor", "Services", "EngineService.cs");
        var macMenuSource = ReadRepositoryFile("Editor", "Platforms", "MacCatalyst", "AppDelegate.cs");
        var nativeSource = ReadRepositoryFile("Runtime", "Sailor.cpp");
        var schedulerHeader = ReadRepositoryFile("Runtime", "Tasks", "Scheduler.h");
        var schedulerSource = ReadRepositoryFile("Runtime", "Tasks", "Scheduler.cpp");
        var windowsSource = ReadRepositoryFile("Lib", "DllMain.cpp");
        var unixSource = ReadRepositoryFile("Lib", "InteropExports.cpp");

        Assert.Contains("extern bool RequestAssetReload()", managedSource, StringComparison.Ordinal);
        Assert.Contains("InvokeRunningInterop(EngineAppInterop.RequestAssetReload)", managedSource, StringComparison.Ordinal);
        Assert.Contains("SAILOR_API bool RequestAssetReload()", windowsSource, StringComparison.Ordinal);
        Assert.Contains("return Sailor::App::RequestAssetReload();", windowsSource, StringComparison.Ordinal);
        Assert.Contains("SAILOR_API bool RequestAssetReload()", unixSource, StringComparison.Ordinal);
        Assert.Contains("return Sailor::App::RequestAssetReload();", unixSource, StringComparison.Ordinal);

        var f5Request = nativeSource.IndexOf("if (systemInputState.IsKeyPressed(VK_F5))", StringComparison.Ordinal);
        var queueRequest = nativeSource.IndexOf("RequestAssetReload();", f5Request, StringComparison.Ordinal);
        var engineThreadApply = nativeSource.IndexOf("ApplyPendingAssetReloadOnEngineThread();", queueRequest, StringComparison.Ordinal);
        var shaderCacheRecovery = nativeSource.IndexOf("shaderCompiler->RecoverMissingShaderCacheStorage();", engineThreadApply, StringComparison.Ordinal);
        var queuedScan = nativeSource.IndexOf("assetRegistry->ScanContentFolder();", engineThreadApply, StringComparison.Ordinal);
        Assert.True(f5Request >= 0);
        Assert.True(queueRequest > f5Request);
        Assert.True(engineThreadApply > queueRequest);
        Assert.True(shaderCacheRecovery > engineThreadApply);
        Assert.True(queuedScan > shaderCacheRecovery);
        Assert.Contains("g_assetReloadRequested.store(true", nativeSource, StringComparison.Ordinal);
        Assert.Contains("g_assetReloadRequested.exchange(false", nativeSource, StringComparison.Ordinal);

        var appStart = nativeSource.IndexOf("void App::Start()", StringComparison.Ordinal);
        var attachEngineThread = nativeSource.IndexOf("scheduler->AttachCurrentThreadAsMainThread();", appStart, StringComparison.Ordinal);
        var frameLoop = nativeSource.IndexOf("while (pMainWindow->IsRunning())", attachEngineThread, StringComparison.Ordinal);
        var appShutdown = nativeSource.IndexOf("void App::Shutdown()", StringComparison.Ordinal);
        var attachShutdownThread = nativeSource.IndexOf("scheduler->AttachCurrentThreadAsMainThread();", appShutdown, StringComparison.Ordinal);
        Assert.True(attachEngineThread > appStart);
        Assert.True(frameLoop > attachEngineThread);
        Assert.True(attachShutdownThread > appShutdown);
        Assert.Contains("std::atomic<DWORD> m_mainThreadId", schedulerHeader, StringComparison.Ordinal);
        Assert.Contains("void Scheduler::AttachCurrentThreadAsMainThread()", schedulerSource, StringComparison.Ordinal);
        Assert.Contains("m_mainThreadId.store(GetCurrentThreadId()", schedulerSource, StringComparison.Ordinal);
        Assert.Contains("return GetMainThreadId() == GetCurrentThreadId();", schedulerSource, StringComparison.Ordinal);

        Assert.Contains("ReloadAssetsSelector", macMenuSource, StringComparison.Ordinal);
        Assert.Contains("UIKeyModifierFlags.Command", macMenuSource, StringComparison.Ordinal);
        Assert.Contains("\"r\"", macMenuSource, StringComparison.Ordinal);
        Assert.Contains("MauiProgram.GetService<EngineService>().RequestAssetReload()", macMenuSource, StringComparison.Ordinal);
        Assert.DoesNotContain("ScanContentFolder", macMenuSource, StringComparison.Ordinal);
    }

    [Fact]
    public void WorkspaceCacheIdentity_IsAvailableOnBothExportSurfacesAndManagedInterop()
    {
        var managedSource = ReadRepositoryFile("Editor", "Services", "EngineService.cs");
        var windowsSource = ReadRepositoryFile("Lib", "DllMain.cpp");
        var unixSource = ReadRepositoryFile("Lib", "InteropExports.cpp");

        Assert.Contains("extern uint SerializeWorkspaceCacheIdentity", managedSource, StringComparison.Ordinal);
        Assert.Contains(
            "yaml = Marshal.PtrToStringUTF8(yamlNodeChar[0], (int)numChars)",
            managedSource,
            StringComparison.Ordinal);
        Assert.Contains("SAILOR_API uint32_t SerializeWorkspaceCacheIdentity", windowsSource, StringComparison.Ordinal);
        Assert.Contains("return Sailor::App::SerializeWorkspaceCacheIdentity(yamlNode);", windowsSource, StringComparison.Ordinal);
        Assert.Contains("SAILOR_API uint32_t SerializeWorkspaceCacheIdentity", unixSource, StringComparison.Ordinal);
        Assert.Contains("return Sailor::App::SerializeWorkspaceCacheIdentity(yamlNode);", unixSource, StringComparison.Ordinal);
    }

    [Fact]
    public void ManagedInitialization_MarshalsEveryCommandLineTokenAsUtf8AndAlwaysFreesIt()
    {
        var managedSource = ReadRepositoryFile("Editor", "Services", "EngineService.cs");
        var nativeSource = ReadRepositoryFile("Runtime", "Sailor.cpp");

        Assert.Contains(
            "EntryPoint = \"Initialize\", ExactSpelling = true",
            managedSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "static extern void InitializeNative([In] nint[] commandLineArgs, int num)",
            managedSource,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "extern void Initialize(string[] commandLineArgs",
            managedSource,
            StringComparison.Ordinal);

        var allocation = managedSource.IndexOf(
            "Utf8InteropArguments.Allocate(commandLineArgs, num)",
            StringComparison.Ordinal);
        var invocation = managedSource.IndexOf("InitializeNative(nativeArguments, num)", allocation, StringComparison.Ordinal);
        var finallyBlock = managedSource.IndexOf("finally", invocation, StringComparison.Ordinal);
        var cleanup = managedSource.IndexOf("Utf8InteropArguments.Free(nativeArguments)", finallyBlock, StringComparison.Ordinal);
        Assert.True(allocation >= 0);
        Assert.True(invocation > allocation);
        Assert.True(finallyBlock > invocation);
        Assert.True(cleanup > finallyBlock);

        Assert.Contains(
            "return Workspace::PathFromUtf8(params.m_workspace);",
            nativeSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "Workspace::PathFromUtf8(params.m_workspaceManifest)",
            nativeSource,
            StringComparison.Ordinal);
    }

    [Fact]
    public void EditorTypeCache_IsValidatedAfterNativeInitializationAndBeforeLivePublication()
    {
        var source = ReadRepositoryFile("Editor", "Services", "EngineService.cs");

        var initialize = source.IndexOf("EngineAppInterop.Initialize(args, args.Length)", StringComparison.Ordinal);
        var identity = source.IndexOf("ReadWorkspaceCacheIdentity(", initialize, StringComparison.Ordinal);
        var cacheLoad = source.IndexOf("editorTypeCacheStore.Load(", identity, StringComparison.Ordinal);
        var cachedValidation = source.IndexOf("TryParseEditorTypes(", cacheLoad, StringComparison.Ordinal);
        var liveCatalog = source.IndexOf("SerializeEditorTypes(generation, allowStarting: true)", cacheLoad, StringComparison.Ordinal);
        var liveValidation = source.IndexOf("TryParseEditorTypes(", liveCatalog, StringComparison.Ordinal);
        var livePublication = source.IndexOf("Volatile.Write(ref editorTypes, liveEditorTypes)", liveValidation, StringComparison.Ordinal);
        var cacheSave = source.IndexOf("editorTypeCacheStore.Save(", liveCatalog, StringComparison.Ordinal);

        Assert.True(initialize >= 0);
        Assert.True(identity > initialize);
        Assert.True(cacheLoad > identity);
        Assert.True(cachedValidation > cacheLoad);
        Assert.True(liveCatalog > cachedValidation);
        Assert.True(liveCatalog > cacheLoad);
        Assert.True(liveValidation > liveCatalog);
        Assert.True(livePublication > liveValidation);
        Assert.True(cacheSave > liveCatalog);
        Assert.Contains(
            "cachedEditorTypes.Status != EditorTypeCacheStatus.IoFailure",
            source,
            StringComparison.Ordinal);
        Assert.Contains(
            "EditorTypeCacheStore.ShouldPersistLiveCatalog(cachedEditorTypes.Status)",
            source,
            StringComparison.Ordinal);
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
