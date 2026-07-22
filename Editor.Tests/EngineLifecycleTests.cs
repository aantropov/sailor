namespace Editor.Tests;

public sealed class EngineLifecycleTests
{
    [Fact]
    public void EditorBundle_CompilesCriticalXamlEntryPoints()
    {
        var project = ReadRepositoryFile("Editor", "SailorEditor.csproj");

        AssertMauiXamlGenerator(project, "App.xaml");
        AssertMauiXamlGenerator(project, "AppShell.xaml");
        AssertMauiXamlGenerator(project, "Views\\InspectorView\\FrameGraphFileTemplate.xaml");
    }

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
    public void NativeLifecycle_CreatesAndDestroysPlatformWindowsInsideMainThreadInteropLock()
    {
        var source = ReadRepositoryFile("Editor", "Services", "EngineService.cs");

        var start = source.IndexOf("public async Task StartAsync(", StringComparison.Ordinal);
        var initialize = source.IndexOf("EngineAppInterop.Initialize(args, args.Length);", start, StringComparison.Ordinal);
        var initializeDispatch = source.LastIndexOf("await MainThread.InvokeOnMainThreadAsync", initialize, StringComparison.Ordinal);
        var initializeLock = source.LastIndexOf("lock (interopLock)", initialize, StringComparison.Ordinal);
        Assert.True(start >= 0);
        Assert.True(initializeDispatch > start);
        Assert.True(initializeLock > initializeDispatch);
        Assert.True(initialize > initializeLock);

        var complete = source.IndexOf("async Task CompleteSessionAsync", StringComparison.Ordinal);
        var completeShutdown = source.IndexOf("await ShutdownNativeSessionAsync(", complete, StringComparison.Ordinal);
        Assert.True(complete >= 0);
        Assert.True(completeShutdown > complete);

        var failedStart = source.IndexOf("async Task ShutdownNativeAfterFailedStartAsync", StringComparison.Ordinal);
        var failedStartShutdown = source.IndexOf("await ShutdownNativeSessionAsync(", failedStart, StringComparison.Ordinal);
        Assert.True(failedStart >= 0);
        Assert.True(failedStartShutdown > failedStart);

        var dispatchHelper = source.IndexOf("Task<Exception?> ShutdownNativeSessionAsync", StringComparison.Ordinal);
        var shutdownDispatch = source.IndexOf("return MainThread.InvokeOnMainThreadAsync", dispatchHelper, StringComparison.Ordinal);
        var shutdownDispatchCall = source.IndexOf("ShutdownNativeSessionUnderLock", shutdownDispatch, StringComparison.Ordinal);
        var lockedHelper = source.IndexOf("Exception? ShutdownNativeSessionUnderLock", shutdownDispatchCall, StringComparison.Ordinal);
        var shutdownLock = source.IndexOf("lock (interopLock)", lockedHelper, StringComparison.Ordinal);
        var nativeShutdown = source.IndexOf("EngineAppInterop.Shutdown();", shutdownLock, StringComparison.Ordinal);
        Assert.True(dispatchHelper >= 0);
        Assert.True(shutdownDispatch > dispatchHelper);
        Assert.True(shutdownDispatchCall > shutdownDispatch);
        Assert.True(lockedHelper > shutdownDispatchCall);
        Assert.True(shutdownLock > lockedHelper);
        Assert.True(nativeShutdown > shutdownLock);
    }

    [Fact]
    public void MacHostBinding_IsAcknowledgedPerGenerationAndRetriedUntilApplied()
    {
        var source = ReadRepositoryFile("Editor", "Services", "EngineService.cs");
        var lifecycleSource = ReadRepositoryFile("Editor", "Scene", "SceneViewportLifecycle.cs");

        Assert.Contains("Dictionary<ulong, (long Generation, nint Handle)> appliedMacRemoteViewportHosts", source, StringComparison.Ordinal);

        var bind = source.IndexOf("public void BindMacRemoteViewportHost", StringComparison.Ordinal);
        var generation = source.IndexOf("Volatile.Read(ref engineGeneration)", bind, StringComparison.Ordinal);
        var runningGuard = source.IndexOf("if (!IsInteropRunningUnderLock())", generation, StringComparison.Ordinal);
        var removeWhileStopped = source.IndexOf("appliedMacRemoteViewportHosts.Remove(viewportId);", runningGuard, StringComparison.Ordinal);
        var nativeBind = source.IndexOf("EngineAppInterop.SetRemoteViewportMacHostHandle", removeWhileStopped, StringComparison.Ordinal);
        var acknowledge = source.IndexOf("appliedMacRemoteViewportHosts[viewportId] = (generation, hostHandle);", nativeBind, StringComparison.Ordinal);
        Assert.True(bind >= 0);
        Assert.True(generation > bind);
        Assert.True(runningGuard > generation);
        Assert.True(removeWhileStopped > runningGuard);
        Assert.True(nativeBind > removeWhileStopped);
        Assert.True(acknowledge > nativeBind);

        var sync = lifecycleSource.IndexOf("public bool Sync", StringComparison.Ordinal);
        var announceHost = lifecycleSource.IndexOf("backend.BindMacHost(viewportId, frame.NativeHostHandle);", sync, StringComparison.Ordinal);
        var updateViewport = lifecycleSource.IndexOf("backend.TryUpdateViewport", announceHost, StringComparison.Ordinal);
        Assert.True(sync >= 0);
        Assert.True(announceHost > sync);
        Assert.True(updateViewport > announceHost);
    }

    [Fact]
    public void NativeViewportState_IsResetBeforeRendererShutdown()
    {
        var appSource = ReadRepositoryFile("Runtime", "Sailor.cpp");
        var bridgeSource = ReadRepositoryFile("Runtime", "Editor", "EditorRuntimeBridge.cpp");

        var initialize = appSource.IndexOf("void App::Initialize(", StringComparison.Ordinal);
        var initializeReset = appSource.IndexOf("EditorRuntime::ResetForAppLifecycle();", initialize, StringComparison.Ordinal);
        var instanceCreation = appSource.IndexOf("s_pInstance = new App();", initialize, StringComparison.Ordinal);
        var shutdown = appSource.IndexOf("void App::Shutdown()", StringComparison.Ordinal);
        var reset = appSource.IndexOf("EditorRuntime::ResetForAppLifecycle();", shutdown, StringComparison.Ordinal);
        var rendererShutdown = appSource.IndexOf("renderer->BeginConditionalDestroy();", shutdown, StringComparison.Ordinal);
        Assert.True(initialize >= 0);
        Assert.True(initializeReset > initialize);
        Assert.True(instanceCreation > initializeReset);
        Assert.True(shutdown >= 0);
        Assert.True(reset > shutdown);
        Assert.True(rendererShutdown > reset);

        Assert.Contains("g_remoteViewportBindings.Clear();", bridgeSource, StringComparison.Ordinal);
        Assert.Contains("Win32::GlobalInput::Reset();", bridgeSource, StringComparison.Ordinal);
        Assert.Contains("g_pendingRemoteViewportHostHandles.Clear();", bridgeSource, StringComparison.Ordinal);
        Assert.Contains("g_pendingEditorViewport = {};", bridgeSource, StringComparison.Ordinal);
        Assert.Contains("g_appliedEditorRenderArea = { 0, 0 };", bridgeSource, StringComparison.Ordinal);
        Assert.Contains("g_editorRemoteViewportRenderArea = { 0, 0 };", bridgeSource, StringComparison.Ordinal);
        Assert.Contains("g_hasPendingEditorViewport = false;", bridgeSource, StringComparison.Ordinal);
    }

    [Fact]
    public void MacNativeWindow_TeardownReusesEditorSurfaceAndClosesStandalone()
    {
        var header = ReadRepositoryFile("Runtime", "Platform", "Win32", "Window.h");
        var source = ReadRepositoryFile("Runtime", "Platform", "Mac", "Window.mm");

        Assert.Contains("SAILOR_API ~Window() override;", header, StringComparison.Ordinal);

        var destructor = source.IndexOf("Window::~Window()", StringComparison.Ordinal);
        var destructorCleanup = source.IndexOf("Destroy();", destructor, StringComparison.Ordinal);
        Assert.True(destructor >= 0);
        Assert.True(destructorCleanup > destructor);

        var create = source.IndexOf("bool Window::Create(", StringComparison.Ordinal);
        var acquireReusable = source.IndexOf(
            "NSWindow* window = bRunsInsideEditor ? sReusableEditorRenderingWindow : nil;",
            create,
            StringComparison.Ordinal);
        var clearReusable = source.IndexOf("sReusableEditorRenderingWindow = nil;", acquireReusable, StringComparison.Ordinal);
        Assert.True(create >= 0);
        Assert.True(acquireReusable > create);
        Assert.True(clearReusable > acquireReusable);

        var destroy = source.IndexOf("void Window::Destroy()", StringComparison.Ordinal);
        var mainThreadCheck = source.IndexOf("if (![NSThread isMainThread])", destroy, StringComparison.Ordinal);
        var mainThreadDispatch = source.IndexOf("dispatch_sync(dispatch_get_main_queue()", mainThreadCheck, StringComparison.Ordinal);
        var clearHandle = source.IndexOf("m_hWnd = nullptr;", destroy, StringComparison.Ordinal);
        var unregister = source.IndexOf("g_windows.Remove(this);", destroy, StringComparison.Ordinal);
        var clearDelegateTarget = source.IndexOf("delegate.sailorWindow = nullptr;", destroy, StringComparison.Ordinal);
        var detachDelegate = source.IndexOf("window.delegate = nil;", destroy, StringComparison.Ordinal);
        var clearAssociation = source.IndexOf("objc_setAssociatedObject(window, sSailorWindowDelegateKey, nil", destroy, StringComparison.Ordinal);
        var close = source.IndexOf("[window close];", destroy, StringComparison.Ordinal);
        var standaloneRelease = source.IndexOf("[window release];", close, StringComparison.Ordinal);
        var orderOut = source.IndexOf("[window orderOut:nil];", standaloneRelease, StringComparison.Ordinal);
        var preserveReusable = source.IndexOf("sReusableEditorRenderingWindow = window;", orderOut, StringComparison.Ordinal);
        var nativeCloseHandler = source.IndexOf("void Window::HandleNativeWindowWillClose", destroy, StringComparison.Ordinal);

        Assert.True(destroy >= 0);
        Assert.True(mainThreadCheck > destroy);
        Assert.True(mainThreadDispatch > mainThreadCheck);
        Assert.True(clearHandle > mainThreadDispatch);
        Assert.True(unregister > mainThreadDispatch);
        Assert.True(clearDelegateTarget > destroy);
        Assert.True(detachDelegate > clearDelegateTarget);
        Assert.True(clearAssociation > detachDelegate);
        Assert.True(close > clearHandle);
        Assert.True(close > unregister);
        Assert.True(close > clearAssociation);
        Assert.True(standaloneRelease > close);
        Assert.True(orderOut > standaloneRelease);
        Assert.True(preserveReusable > orderOut);
        Assert.True(nativeCloseHandler > preserveReusable);
    }

    [Fact]
    public void DeferredPublications_AreRejectedAfterGenerationChanges()
    {
        var source = ReadRepositoryFile("Editor", "Services", "EngineService.cs");

        Assert.Contains("Interlocked.Increment(ref engineGeneration)", source, StringComparison.Ordinal);
        Assert.Contains("!IsGenerationActive(generation, allowStarting)", source, StringComparison.Ordinal);
        Assert.Contains("!worldSnapshotPublication.TryAdvance(snapshotSequence)", source, StringComparison.Ordinal);
        Assert.Contains("message.Generation == generation", source, StringComparison.Ordinal);
        Assert.Contains(".Where(message => message.Generation == generation)", source, StringComparison.Ordinal);
        Assert.Contains("QueueWorldUpdate", source, StringComparison.Ordinal);
    }

    [Fact]
    public void SuccessfulLocalWorldMutation_AdvancesSnapshotGateUnderTheInteropLock()
    {
        var source = ReadRepositoryFile("Editor", "Services", "EngineService.cs");

        var helper = source.IndexOf("bool InvokeRunningInterop", StringComparison.Ordinal);
        var interopLock = source.IndexOf("lock (interopLock)", helper, StringComparison.Ordinal);
        var nativeAction = source.IndexOf("!action()", interopLock, StringComparison.Ordinal);
        var reserveSequence = source.IndexOf(
            "worldSnapshotPublication.ReserveSequence()",
            nativeAction,
            StringComparison.Ordinal);
        var advanceGate = source.IndexOf(
            "worldSnapshotPublication.TryAdvance(mutationSequence)",
            reserveSequence,
            StringComparison.Ordinal);
        var helperEnd = source.IndexOf("public static void ShowMainWindow", advanceGate, StringComparison.Ordinal);
        var commitChanges = source.IndexOf("public bool CommitChanges", helperEnd, StringComparison.Ordinal);
        var invalidateQueuedSnapshots = source.IndexOf(
            "invalidateQueuedWorldSnapshots: true",
            commitChanges,
            StringComparison.Ordinal);

        Assert.True(helper >= 0);
        Assert.True(interopLock > helper);
        Assert.True(nativeAction > interopLock);
        Assert.True(reserveSequence > nativeAction);
        Assert.True(advanceGate > reserveSequence);
        Assert.True(helperEnd > advanceGate);
        Assert.True(commitChanges > helperEnd);
        Assert.True(invalidateQueuedSnapshots > commitChanges);
    }

    [Fact]
    public void WindowsViewportBootstrap_PreservesTheLatestSceneGeometryVisibilityAndFocus()
    {
        var source = ReadRepositoryFile("Editor", "Services", "EngineService.cs");

        var update = source.IndexOf("public bool TryUpdateRemoteViewport", StringComparison.Ordinal);
        var interopLock = source.IndexOf("lock (interopLock)", update, StringComparison.Ordinal);
        var rememberState = source.IndexOf("sceneViewportState.Remember(rect, visible, focused)", interopLock, StringComparison.Ordinal);
        var refresh = source.IndexOf("bool TryRefreshSceneRemoteViewport", rememberState, StringComparison.Ordinal);
        var refreshLock = source.IndexOf("lock (interopLock)", refresh, StringComparison.Ordinal);
        var captureState = source.IndexOf("var viewportState = sceneViewportState.Capture();", refreshLock, StringComparison.Ordinal);
        var preservedRect = source.IndexOf("viewportState.Rect,", captureState, StringComparison.Ordinal);
        var preservedVisibility = source.IndexOf("viewportState.Visible,", preservedRect, StringComparison.Ordinal);
        var preservedFocus = source.IndexOf("viewportState.Focused", preservedVisibility, StringComparison.Ordinal);
        var bootstrap = source.IndexOf("pollTasks.Add(RunPeriodicTaskAsync(() =>", rememberState, StringComparison.Ordinal);
        var remoteUpdate = source.IndexOf("TryRefreshSceneRemoteViewport(generation)", bootstrap, StringComparison.Ordinal);

        Assert.True(update >= 0);
        Assert.True(interopLock > update);
        Assert.True(rememberState > interopLock);
        Assert.True(refresh > rememberState);
        Assert.True(refreshLock > refresh);
        Assert.True(captureState > refreshLock);
        Assert.True(preservedRect > captureState);
        Assert.True(preservedVisibility > preservedRect);
        Assert.True(preservedFocus > preservedVisibility);
        Assert.True(bootstrap > refresh);
        Assert.True(remoteUpdate > bootstrap);
        Assert.DoesNotContain(
            "TryUpdateRemoteViewport(SceneViewportId, Viewport, visible: true, focused: false)",
            source,
            StringComparison.Ordinal);
        Assert.DoesNotContain("TryRefreshSceneRemoteViewport(Viewport)", source, StringComparison.Ordinal);
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
    public void CreateEditorWorld_IsAvailableOnBothExportSurfacesAndManagedInterop()
    {
        var managedSource = ReadRepositoryFile("Editor", "Services", "EngineService.cs");
        var windowsSource = ReadRepositoryFile("Lib", "DllMain.cpp");
        var unixSource = ReadRepositoryFile("Lib", "InteropExports.cpp");

        Assert.Contains("extern bool CreateEditorWorld()", managedSource, StringComparison.Ordinal);
        Assert.Contains("SAILOR_API bool CreateEditorWorld()", windowsSource, StringComparison.Ordinal);
        Assert.Contains("return Sailor::App::CreateEditorWorld();", windowsSource, StringComparison.Ordinal);
        Assert.Contains("SAILOR_API bool CreateEditorWorld()", unixSource, StringComparison.Ordinal);
        Assert.Contains("return Sailor::App::CreateEditorWorld();", unixSource, StringComparison.Ordinal);
    }

    static void AssertMauiXamlGenerator(string project, string xamlPath)
    {
        var itemStart = project.IndexOf($"<MauiXaml Update=\"{xamlPath}\">", StringComparison.Ordinal);
        var itemEnd = project.IndexOf("</MauiXaml>", itemStart, StringComparison.Ordinal);

        Assert.True(itemStart >= 0, $"Missing MauiXaml item for {xamlPath}.");
        Assert.True(itemEnd > itemStart, $"Malformed MauiXaml item for {xamlPath}.");
        Assert.Contains(
            "<Generator>MSBuild:Compile</Generator>",
            project[itemStart..itemEnd],
            StringComparison.Ordinal);
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

        var reloadBody = nativeSource.IndexOf("bool ReloadAssetsOnEngineMainThread()", StringComparison.Ordinal);
        Assert.True(reloadBody >= 0);

        var reloadBodyEnd = nativeSource.IndexOf("bool App::DispatchOnEngineMainThread", reloadBody, StringComparison.Ordinal);
        Assert.True(reloadBodyEnd > reloadBody);

        var initialWait = nativeSource.IndexOf("scheduler->WaitIdle({", reloadBody, StringComparison.Ordinal);
        Assert.True(initialWait > reloadBody);

        var shaderCacheRecovery = nativeSource.IndexOf("shaderCompiler->RecoverMissingShaderCacheStorage();", initialWait, StringComparison.Ordinal);
        Assert.True(shaderCacheRecovery > initialWait);

        var queuedScan = nativeSource.IndexOf("assetRegistry->ScanContentFolder();", shaderCacheRecovery, StringComparison.Ordinal);
        Assert.True(queuedScan > shaderCacheRecovery);

        var completionWait = nativeSource.IndexOf("scheduler->WaitIdle({", queuedScan, StringComparison.Ordinal);
        Assert.True(completionWait > queuedScan);

        var scanProcessing = nativeSource.IndexOf("assetRegistry->CompleteScanProcessing();", completionWait, StringComparison.Ordinal);
        Assert.True(scanProcessing > completionWait);

        var frameGraphRefresh = nativeSource.IndexOf("renderer->RefreshFrameGraph();", scanProcessing, StringComparison.Ordinal);
        Assert.True(frameGraphRefresh > scanProcessing);

        var completionWaitBody = nativeSource[completionWait..scanProcessing];
        Assert.Contains("EThreadType::Worker", completionWaitBody, StringComparison.Ordinal);
        Assert.Contains("EThreadType::Render", completionWaitBody, StringComparison.Ordinal);
        Assert.Contains("EThreadType::RHI", completionWaitBody, StringComparison.Ordinal);
        Assert.DoesNotContain("EThreadType::Main", nativeSource[reloadBody..reloadBodyEnd], StringComparison.Ordinal);

        var f5Request = nativeSource.IndexOf("if (systemInputState.IsKeyPressed(VK_F5))", StringComparison.Ordinal);
        var queuedReload = nativeSource.IndexOf("RequestAssetReload();", f5Request, StringComparison.Ordinal);
        Assert.True(f5Request >= 0);
        Assert.True(queuedReload > f5Request);
        Assert.Contains("consoleVars[\"scan\"] = []() { App::RequestAssetReload(); };", nativeSource, StringComparison.Ordinal);

        var requestStart = nativeSource.IndexOf("bool App::RequestAssetReload()", StringComparison.Ordinal);
        var requestProcessor = nativeSource.IndexOf("void App::ProcessAssetReloadRequestOnEngineMainThread()", requestStart, StringComparison.Ordinal);
        var requestEnd = nativeSource.IndexOf("void App::Shutdown()", requestProcessor, StringComparison.Ordinal);
        var requestBody = nativeSource[requestStart..requestProcessor];
        var requestProcessorBody = nativeSource[requestProcessor..requestEnd];
        Assert.DoesNotContain("std::try_to_lock", requestBody, StringComparison.Ordinal);
        Assert.DoesNotContain("g_engineMainLoopRunning", requestBody, StringComparison.Ordinal);
        Assert.Contains("g_engineMainLoopState == EEngineMainLoopState::Exited", requestBody, StringComparison.Ordinal);
        Assert.Contains("++s_pInstance->m_assetReloadRequestGeneration", requestBody, StringComparison.Ordinal);
        Assert.Contains("!s_pInstance->m_pendingAssetReloadTask->IsFinished()", requestBody, StringComparison.Ordinal);
        Assert.DoesNotContain("m_pendingAssetReloadTask->IsStarted()", requestBody, StringComparison.Ordinal);
        Assert.Contains("requestGeneration = s_pInstance->m_assetReloadRequestGeneration", requestProcessorBody, StringComparison.Ordinal);
        Assert.Contains("m_assetReloadRequestGeneration == requestGeneration", requestProcessorBody, StringComparison.Ordinal);
        Assert.Contains("\"Reload assets on engine main thread\"", requestBody, StringComparison.Ordinal);
        Assert.Contains("EThreadType::Main", requestBody, StringComparison.Ordinal);
        Assert.Contains("scheduler->Run(task);", requestBody, StringComparison.Ordinal);
        Assert.Contains("QueueAssetReloadTaskLocked(scheduler);", requestProcessorBody, StringComparison.Ordinal);
        Assert.DoesNotContain("task->Wait()", nativeSource[requestStart..requestEnd], StringComparison.Ordinal);
        Assert.DoesNotContain("g_assetReloadRequested", nativeSource, StringComparison.Ordinal);
        Assert.DoesNotContain("ApplyPendingAssetReloadOnEngineThread", nativeSource, StringComparison.Ordinal);

        var appStart = nativeSource.IndexOf("void App::Start()", StringComparison.Ordinal);
        var attachEngineThread = nativeSource.IndexOf("scheduler->AttachCurrentThreadAsMainThread();", appStart, StringComparison.Ordinal);
        var frameLoop = nativeSource.IndexOf("while (pMainWindow->IsRunning())", attachEngineThread, StringComparison.Ordinal);
        var appShutdown = nativeSource.IndexOf("void App::Shutdown()", StringComparison.Ordinal);
        var attachShutdownThread = nativeSource.IndexOf("scheduler->AttachCurrentThreadAsMainThread();", appShutdown, StringComparison.Ordinal);
        Assert.True(attachEngineThread > appStart);
        Assert.True(frameLoop > attachEngineThread);
        Assert.Contains("m_assetReloadRequestGeneration > 0", nativeSource[appStart..frameLoop], StringComparison.Ordinal);
        Assert.Contains("QueueAssetReloadTaskLocked(scheduler);", nativeSource[appStart..frameLoop], StringComparison.Ordinal);
        Assert.True(attachShutdownThread > appShutdown);
        Assert.Contains("std::atomic<DWORD> m_mainThreadId", schedulerHeader, StringComparison.Ordinal);
        Assert.Contains("void Scheduler::AttachCurrentThreadAsMainThread()", schedulerSource, StringComparison.Ordinal);
        Assert.Contains("m_mainThreadId.store(GetCurrentThreadId()", schedulerSource, StringComparison.Ordinal);
        Assert.Contains("return GetMainThreadId() == GetCurrentThreadId();", schedulerSource, StringComparison.Ordinal);

        Assert.Contains("ReloadAssetsSelector", macMenuSource, StringComparison.Ordinal);
        Assert.Contains("UIKeyModifierFlags.Command", macMenuSource, StringComparison.Ordinal);
        Assert.Contains("\"r\"", macMenuSource, StringComparison.Ordinal);
        Assert.Contains("MauiProgram.GetService<EngineService>().RequestAssetReloadAsync()", macMenuSource, StringComparison.Ordinal);
        Assert.DoesNotContain("ScanContentFolder", macMenuSource, StringComparison.Ordinal);
    }

    [Fact]
    public void AssetReload_RefreshesTheEditorProjectionOnlyAfterNativeCommit()
    {
        var managedSource = ReadRepositoryFile("Editor", "Services", "EngineService.cs");
        var assetsSource = ReadRepositoryFile("Editor", "Services", "AssetsService.cs");
        var nativeSource = ReadRepositoryFile("Runtime", "Sailor.cpp");
        var registryHeader = ReadRepositoryFile("Runtime", "AssetRegistry", "AssetRegistry.h");
        var windowsSource = ReadRepositoryFile("Lib", "DllMain.cpp");
        var unixSource = ReadRepositoryFile("Lib", "InteropExports.cpp");

        Assert.Contains("extern bool GetAssetReloadState(out ulong requestGeneration, out ulong completedGeneration, out ulong successfulGeneration)", managedSource, StringComparison.Ordinal);
        Assert.Contains("SAILOR_API bool GetAssetReloadState(", windowsSource, StringComparison.Ordinal);
        Assert.Contains("SAILOR_API bool GetAssetReloadState(", unixSource, StringComparison.Ordinal);
        Assert.Contains("public event Action<AssetReloadCompletion> OnAssetReloadCompleted", managedSource, StringComparison.Ordinal);
        Assert.Contains("PublishAssetReloadCompletion(", managedSource, StringComparison.Ordinal);
        Assert.Contains("successfulReloadGeneration == completedReloadGeneration", managedSource, StringComparison.Ordinal);

        Assert.Contains("SAILOR_API bool ScanContentFolder();", registryHeader, StringComparison.Ordinal);
        Assert.Contains("m_assetReloadCompletedGeneration", nativeSource, StringComparison.Ordinal);
        Assert.Contains("m_assetReloadSuccessfulGeneration", nativeSource, StringComparison.Ordinal);
        Assert.Contains("const bool bReloaded = assetRegistry->ScanContentFolder();", nativeSource, StringComparison.Ordinal);

        Assert.Contains("engineService.OnAssetReloadCompleted += HandleAssetReloadCompleted;", assetsSource, StringComparison.Ordinal);
        Assert.Contains("if (!completion.Succeeded || _activeLaunchContext is null)", assetsSource, StringComparison.Ordinal);
        Assert.Contains("Refresh();", assetsSource, StringComparison.Ordinal);
        Assert.Contains("selectionService.SelectObject(refreshedAsset, force: true);", assetsSource, StringComparison.Ordinal);
    }

    [Fact]
    public void AssetProjection_TreatsGlslAsShaderLibraryDespiteSharedNativeAssetInfoType()
    {
        var assetsSource = ReadRepositoryFile("Editor", "Services", "AssetsService.cs");
        var reflectionSource = ReadRepositoryFile("Runtime", "Core", "Reflection.cpp");

        Assert.Contains(
            "\"Sailor::ShaderAssetInfo\" when string.Equals(extension, \".glsl\", StringComparison.OrdinalIgnoreCase) => new ShaderLibraryFile()",
            assetsSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "\"Sailor::ShaderAssetInfo\" => new ShaderFile()",
            assetsSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "ExportAssetInfoType<ShaderAssetInfo>(\"Sailor::ShaderAssetInfo\", { \"shader\", \"glsl\" })",
            reflectionSource,
            StringComparison.Ordinal);
    }

    [Fact]
    public void ContentAssetDuplicate_RoutesTheWholeGroupThroughOneRegistryReload()
    {
        var viewSource = ReadRepositoryFile("Editor", "Views", "ContentFolderView.xaml.cs");
        var commandSource = ReadRepositoryFile("Editor", "Commands", "EditorAssetCommands.cs");
        var serviceSource = ReadRepositoryFile("Editor", "Services", "AssetsService.cs");
        var operationsSource = ReadRepositoryFile("Editor", "Content", "ProjectContentFileOperations.cs");

        Assert.Contains("Text = \"Duplicate\"", viewSource, StringComparison.Ordinal);
        Assert.Contains("new DuplicateAssetCommand(assetFile)", viewSource, StringComparison.Ordinal);
        Assert.Contains("public sealed class DuplicateAssetCommand", commandSource, StringComparison.Ordinal);
        Assert.Contains("AssetCommandMessages.CompleteMutationAsync", commandSource, StringComparison.Ordinal);
        Assert.Contains("_fileOperations.DuplicateAssetGroup(", serviceSource, StringComparison.Ordinal);
        Assert.Contains("public ProjectContentFileOperationResult DuplicateAssetGroup(", operationsSource, StringComparison.Ordinal);
        Assert.Contains("result.CreatedFileId", serviceSource, StringComparison.Ordinal);
    }

    [Fact]
    public void PortableAssetSidecars_DoNotContainRuntimeOrCacheState()
    {
        var contentRoot = Path.Combine(ResolveRepositoryRoot(), "Content");
        var sidecars = Directory.EnumerateFiles(contentRoot, "*.asset", SearchOption.AllDirectories).ToArray();
        Assert.NotEmpty(sidecars);

        foreach (var sidecar in sidecars)
        {
            var contents = File.ReadAllText(sidecar);
            Assert.DoesNotContain("assetImportTime:", contents, StringComparison.Ordinal);
            Assert.DoesNotContain("metadataLoadTime:", contents, StringComparison.Ordinal);
            Assert.DoesNotContain("metadataPath:", contents, StringComparison.Ordinal);
        }

        var assetFileSource = ReadRepositoryFile("Editor", "ViewModels", "AssetFile.cs");
        Assert.Contains("RuntimeOnlyAssetInfoFields", assetFileSource, StringComparison.Ordinal);
        Assert.Contains("\"assetImportTime\"", assetFileSource, StringComparison.Ordinal);
        Assert.Contains("\"metadataLoadTime\"", assetFileSource, StringComparison.Ordinal);
        Assert.Contains("\"metadataPath\"", assetFileSource, StringComparison.Ordinal);
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

    [Fact]
    public void MacStartupWorldSerialization_RunsOnTheAttachedNativeSchedulerThreadBeforeStart()
    {
        var source = ReadRepositoryFile("Editor", "Services", "EngineService.cs");

        var startupOrder = source.IndexOf(
            "// Required startup order: combined editor catalog, world, then initial messages.",
            StringComparison.Ordinal);
        var liveCatalog = source.IndexOf(
            "SerializeEditorTypes(generation, allowStarting: true)",
            startupOrder,
            StringComparison.Ordinal);
        var macBranch = source.IndexOf("#if MACCATALYST", liveCatalog, StringComparison.Ordinal);
        var mainThreadDispatch = source.IndexOf(
            "await MainThread.InvokeOnMainThreadAsync(",
            macBranch,
            StringComparison.Ordinal);
        var worldSerialization = source.IndexOf(
            "() => SerializeWorld(generation, out serializedWorldSequence, allowStarting: true)",
            mainThreadDispatch,
            StringComparison.Ordinal);
        var nativeStart = source.IndexOf(
            "Task.Run(EngineAppInterop.Start, CancellationToken.None)",
            worldSerialization,
            StringComparison.Ordinal);

        Assert.True(startupOrder >= 0);
        Assert.True(liveCatalog > startupOrder);
        Assert.True(macBranch > liveCatalog);
        Assert.True(mainThreadDispatch > macBranch);
        Assert.True(worldSerialization > mainThreadDispatch);
        Assert.True(nativeStart > worldSerialization);
    }

    [Fact]
    public void MacMenuBuild_UsesTheAsyncWorkspaceProjectionInsteadOfStartupFileIo()
    {
        var menuSource = ReadRepositoryFile("Editor", "Platforms", "MacCatalyst", "AppDelegate.cs");
        var workspaceSource = ReadRepositoryFile("Editor", "Services", "WorkspaceUiService.cs");

        var recentMenu = menuSource.IndexOf("static UIMenu BuildRecentWorkspacesMenu()", StringComparison.Ordinal);
        var recentMenuEnd = menuSource.IndexOf("static void RunWorkspaceAction", recentMenu, StringComparison.Ordinal);
        Assert.True(recentMenu >= 0);
        Assert.True(recentMenuEnd > recentMenu);

        var recentMenuBody = menuSource[recentMenu..recentMenuEnd];
        Assert.Contains("var recent = recentWorkspaces;", recentMenuBody, StringComparison.Ordinal);
        Assert.DoesNotContain("RecentWorkspaceStore", recentMenuBody, StringComparison.Ordinal);
        Assert.DoesNotContain(".Load()", recentMenuBody, StringComparison.Ordinal);
        Assert.DoesNotContain("MauiProgram.GetService", recentMenuBody, StringComparison.Ordinal);

        var setProjection = workspaceSource.IndexOf("void SetProjection", StringComparison.Ordinal);
        var setProjectionEnd = workspaceSource.IndexOf("static async Task<string?> PickWorkspaceManifestPathAsync", setProjection, StringComparison.Ordinal);
        Assert.True(setProjection >= 0);
        Assert.True(setProjectionEnd > setProjection);
        Assert.Contains(
            "AppDelegate.UpdateRecentWorkspaces(projection.RecentWorkspaces)",
            workspaceSource[setProjection..setProjectionEnd],
            StringComparison.Ordinal);
    }

    [Fact]
    public void MacPersistence_UsesTheSandboxApplicationDataDirectory()
    {
        var workspaceSource = ReadRepositoryFile("Editor", "Workspace", "WorkspaceLifecycle.cs");
        var settingsSource = ReadRepositoryFile("Editor", "Settings", "UnifiedSettingsStore.cs");
        var engineSource = ReadRepositoryFile("Editor", "Services", "EngineService.cs");

        Assert.Contains("#if MACCATALYST", workspaceSource, StringComparison.Ordinal);
        Assert.Contains(
            "Microsoft.Maui.Storage.FileSystem.Current.AppDataDirectory",
            workspaceSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "Microsoft.Maui.Storage.FileSystem.Current.AppDataDirectory",
            settingsSource,
            StringComparison.Ordinal);

        var engineCache = engineSource.IndexOf("public string EngineCacheDirectory", StringComparison.Ordinal);
        var engineWorkingDirectory = engineSource.IndexOf("public string EngineWorkingDirectory", engineCache, StringComparison.Ordinal);
        Assert.True(engineCache >= 0);
        Assert.True(engineWorkingDirectory > engineCache);
        Assert.Contains(
            "Microsoft.Maui.Storage.FileSystem.Current.AppDataDirectory",
            engineSource[engineCache..engineWorkingDirectory],
            StringComparison.Ordinal);
    }

    [Fact]
    public void MacEditorRenderSurface_DoesNotOwnTheMauiApplicationLifetime()
    {
        var source = ReadRepositoryFile("Runtime", "Platform", "Mac", "Window.mm");

        var closeCallback = source.IndexOf("- (void)windowWillClose:", StringComparison.Ordinal);
        var lifetimeSnapshot = source.IndexOf(
            "const BOOL bTerminatesApplicationOnClose = self.terminatesApplicationOnClose;",
            closeCallback,
            StringComparison.Ordinal);
        var lifetimeGuard = source.IndexOf(
            "if (bTerminatesApplicationOnClose)",
            lifetimeSnapshot,
            StringComparison.Ordinal);
        var terminateCall = source.IndexOf("[NSApp terminate:nil]", lifetimeGuard, StringComparison.Ordinal);
        var editorOwnership = source.IndexOf(
            "delegate.terminatesApplicationOnClose = !bRunsInsideEditor;",
            terminateCall,
            StringComparison.Ordinal);

        Assert.True(closeCallback >= 0);
        Assert.True(lifetimeSnapshot > closeCallback);
        Assert.True(lifetimeGuard > lifetimeSnapshot);
        Assert.True(terminateCall > lifetimeGuard);
        Assert.True(editorOwnership > terminateCall);
    }

    static string ReadRepositoryFile(params string[] relativePath)
    {
        var candidate = Path.Combine(relativePath.Prepend(ResolveRepositoryRoot()).ToArray());
        return File.Exists(candidate)
            ? File.ReadAllText(candidate)
            : throw new FileNotFoundException($"Could not find repository file: {Path.Combine(relativePath)}");
    }

    static string ResolveRepositoryRoot()
    {
        var current = new DirectoryInfo(AppContext.BaseDirectory);
        while (current is not null)
        {
            if (Directory.Exists(Path.Combine(current.FullName, "Content")) &&
                Directory.Exists(Path.Combine(current.FullName, "Editor")) &&
                Directory.Exists(Path.Combine(current.FullName, "Runtime")))
            {
                return current.FullName;
            }

            current = current.Parent;
        }

        throw new DirectoryNotFoundException("Could not find the Sailor repository root.");
    }
}
