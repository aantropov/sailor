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
        Assert.Contains("IAsyncDisposable", source, StringComparison.Ordinal);
        Assert.Contains("public ValueTask DisposeAsync()", source, StringComparison.Ordinal);
        Assert.Contains("disposeTask = Task.Run(DisposeCoreAsync);", source, StringComparison.Ordinal);
        Assert.Contains(
            "await protocolClient.DisposeAsync().ConfigureAwait(false);",
            source,
            StringComparison.Ordinal);
        Assert.Contains("public void ResetForWorkspaceChange()", source, StringComparison.Ordinal);
        Assert.Contains("await session.RuntimeMonitorTask.ConfigureAwait(false)", source, StringComparison.Ordinal);
        Assert.Contains("await Task.WhenAll(session.PollTasks).ConfigureAwait(false)", source, StringComparison.Ordinal);
        Assert.Contains(
            "await protocolClient.ShutdownAsync().ConfigureAwait(false);",
            source,
            StringComparison.Ordinal);
        Assert.Contains("BuildInteropArgumentsAsync", source, StringComparison.Ordinal);
        Assert.Contains("MainThread.InvokeOnMainThreadAsync", source, StringComparison.Ordinal);
    }

    [Fact]
    public void ManagedLifecycle_DisposalIsNonBlockingAndPathTracingIsSessionCancelled()
    {
        var source = ReadRepositoryFile("Editor", "Services", "EngineService.cs");
        var macAppDelegate = ReadRepositoryFile(
            "Editor",
            "Platforms",
            "MacCatalyst",
            "AppDelegate.cs");

        var beginDispose = source.IndexOf("Task BeginDisposeAsync()", StringComparison.Ordinal);
        var disposeCore = source.IndexOf("async Task DisposeCoreAsync()", beginDispose, StringComparison.Ordinal);
        var syncDispose = source.IndexOf("public void Dispose()", disposeCore, StringComparison.Ordinal);
        var asyncDispose = source.IndexOf("public ValueTask DisposeAsync()", syncDispose, StringComparison.Ordinal);
        Assert.True(beginDispose >= 0);
        Assert.True(disposeCore > beginDispose);
        Assert.True(syncDispose > disposeCore);
        Assert.True(asyncDispose > syncDispose);
        Assert.DoesNotContain(
            "GetAwaiter().GetResult()",
            source[beginDispose..],
            StringComparison.Ordinal);

        var terminate = macAppDelegate.IndexOf(
            "public override void WillTerminate",
            StringComparison.Ordinal);
        var disposeAsync = macAppDelegate.IndexOf(
            ".DisposeAsync()",
            terminate,
            StringComparison.Ordinal);
        var asTask = macAppDelegate.IndexOf(
            ".AsTask();",
            disposeAsync,
            StringComparison.Ordinal);
        var pumpMainRunLoop = macAppDelegate.IndexOf(
            "NSRunLoop.Main.RunUntil(deadline);",
            asTask,
            StringComparison.Ordinal);
        var observeShutdown = macAppDelegate.IndexOf(
            "shutdownTask.GetAwaiter().GetResult();",
            pumpMainRunLoop,
            StringComparison.Ordinal);
        var terminateBase = macAppDelegate.IndexOf(
            "base.WillTerminate(application);",
            observeShutdown,
            StringComparison.Ordinal);
        Assert.True(terminate >= 0);
        Assert.True(disposeAsync > terminate);
        Assert.True(asTask > disposeAsync);
        Assert.True(pumpMainRunLoop > asTask);
        Assert.True(observeShutdown > pumpMainRunLoop);
        Assert.True(terminateBase > observeShutdown);

        var pathTrace = source.IndexOf(
            "public async Task<bool> ExportPathTracedImageAsync",
            StringComparison.Ordinal);
        var runWorld = source.IndexOf("public void RunWorld", pathTrace, StringComparison.Ordinal);
        Assert.True(pathTrace >= 0);
        Assert.True(runWorld > pathTrace);
        var pathTraceBody = source[pathTrace..runWorld];
        Assert.Contains(
            "backgroundCancellationToken",
            pathTraceBody,
            StringComparison.Ordinal);
        Assert.Contains(
            "session?.BackgroundCancellation.Token ?? default",
            pathTraceBody,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "InvokeRunningInterop",
            pathTraceBody,
            StringComparison.Ordinal);
    }

    [Fact]
    public void ManagedTransport_LivenessMonitorUsesBoundedProtocolPolls()
    {
        var serviceSource = ReadRepositoryFile(
            "Editor",
            "Services",
            "EngineService.cs");
        var transportSource = ReadRepositoryFile(
            "Editor",
            "Protocol",
            "ClientWebSocketEngineProtocolTransport.cs");

        Assert.Contains(
            "lane.AllowsBlockingRequest",
            transportSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "new(\"background\", allowsBlockingRequest: true)",
            transportSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "? Timeout.InfiniteTimeSpan",
            transportSource,
            StringComparison.Ordinal);
        Assert.Contains(
            ": KeepAliveTimeout;",
            transportSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "await protocolClient.StartAsync(",
            serviceSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "MonitorEngineLifetimeAsync(",
            serviceSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "RuntimeLivenessPollInterval",
            serviceSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "protocolClient.IsEngineRunningAsync(",
            serviceSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "session.RuntimeMonitorCancellation.Cancel();",
            serviceSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "pollCancellation.Token",
            serviceSource,
            StringComparison.Ordinal);

        var stop = serviceSource.IndexOf(
            "public async Task<int> StopAsync",
            StringComparison.Ordinal);
        var orderlyStop = serviceSource.IndexOf(
            "stopFailure = await RequestNativeStopAsync()",
            stop,
            StringComparison.Ordinal);
        var cancelMonitor = serviceSource.IndexOf(
            "session.RuntimeMonitorCancellation.Cancel();",
            stop,
            StringComparison.Ordinal);
        Assert.True(stop >= 0);
        Assert.True(cancelMonitor > stop);
        Assert.True(orderlyStop > stop);
        Assert.True(orderlyStop > cancelMonitor);
    }

    [Fact]
    public void ManagedTeardown_HasBoundedCompletionAndLaneDrains()
    {
        var serviceSource = ReadRepositoryFile(
            "Editor",
            "Services",
            "EngineService.cs");
        var transportSource = ReadRepositoryFile(
            "Editor",
            "Protocol",
            "ClientWebSocketEngineProtocolTransport.cs");

        var disposeCore = serviceSource.IndexOf(
            "async Task DisposeCoreAsync()",
            StringComparison.Ordinal);
        var syncDispose = serviceSource.IndexOf(
            "public void Dispose()",
            disposeCore,
            StringComparison.Ordinal);
        Assert.True(disposeCore >= 0);
        Assert.True(syncDispose > disposeCore);
        var disposeBody = serviceSource[disposeCore..syncDispose];
        Assert.Contains(
            "StopAsync(orderlyStopCancellation.Token)",
            disposeBody,
            StringComparison.Ordinal);
        Assert.Contains(
            "DisposeOrderlyStopTimeout",
            disposeBody,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "StopAsync(CancellationToken.None)",
            disposeBody,
            StringComparison.Ordinal);
        Assert.Contains(
            "completionTask.WaitAsync(",
            disposeBody,
            StringComparison.Ordinal);
        Assert.Contains(
            "DisposeCompletionTimeout",
            disposeBody,
            StringComparison.Ordinal);
        Assert.Contains(
            "DisposeProtocolClientOnce();",
            disposeBody,
            StringComparison.Ordinal);
        Assert.Contains(
            "DisposeAbortDrainTimeout",
            disposeBody,
            StringComparison.Ordinal);
        Assert.Contains(
            "DisposePlatformQueueTimeout",
            disposeBody,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "await completionTask.ConfigureAwait(false);",
            disposeBody,
            StringComparison.Ordinal);

        Assert.Contains(
            "public async ValueTask DisposeAsync()",
            transportSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "await disposalCompletion.Task",
            transportSource,
            StringComparison.Ordinal);
        Assert.Contains(
            ".WaitAsync(DisposeDrainTimeout)",
            transportSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "disposeCancellation.Cancel();",
            transportSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "disposalCompletion.TrySetResult()",
            transportSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "ReleaseDisposalResources();",
            transportSource,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            ".GetAwaiter().GetResult()",
            transportSource,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            ".Gate.Wait(",
            transportSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "Interlocked.Exchange(",
            transportSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "ref lane.Socket",
            transportSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "Volatile.Read(ref activeInvocations)",
            transportSource,
            StringComparison.Ordinal);
    }

    [Fact]
    public void ViewportHotPaths_AreBoundedQueuedAndKeyedPerViewport()
    {
        var source = ReadRepositoryFile(
            "Editor",
            "Services",
            "EngineService.cs");

        Assert.Contains(
            "KeyedLatestQueuedCommand<ulong, RemoteViewportUpdate>",
            source,
            StringComparison.Ordinal);
        Assert.Contains(
            "KeyedLatestQueuedCommand<ulong, RemoteViewportInput>",
            source,
            StringComparison.Ordinal);
        Assert.Contains(
            "remoteViewportUpdates.Enqueue(",
            source,
            StringComparison.Ordinal);
        Assert.Contains(
            "pointerMoves.Enqueue(",
            source,
            StringComparison.Ordinal);
        Assert.Contains(
            "QueueRemoteViewportStatusRefresh(viewportId);",
            source,
            StringComparison.Ordinal);

        var reload = source.IndexOf(
            "public async Task<bool> RequestAssetReloadAsync",
            StringComparison.Ordinal);
        var reloadEnd = source.IndexOf(
            "public async Task RefreshCurrentWorldAsync",
            reload,
            StringComparison.Ordinal);
        Assert.True(reload >= 0);
        Assert.True(reloadEnd > reload);
        Assert.Contains(
            "await protocolClient.RequestAssetReloadAsync(",
            source[reload..reloadEnd],
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "Task.Run(",
            source[reload..reloadEnd],
            StringComparison.Ordinal);
    }

    [Fact]
    public void NativeLifecycle_InitializesOnMainThreadAndShutsDownOffTheUiThread()
    {
        var source = ReadRepositoryFile("Editor", "Services", "EngineService.cs");

        var start = source.IndexOf("public async Task StartAsync(", StringComparison.Ordinal);
        var initializeDispatch = source.IndexOf(
            "await MainThread.InvokeOnMainThreadAsync(",
            start,
            StringComparison.Ordinal);
        var initialize = source.IndexOf(
            "() => protocolClient.InitializeAsync(",
            initializeDispatch,
            StringComparison.Ordinal);
        Assert.True(start >= 0);
        Assert.True(initializeDispatch > start);
        Assert.True(initialize > initializeDispatch);

        var complete = source.IndexOf("async Task CompleteSessionAsync", StringComparison.Ordinal);
        var completeShutdown = source.IndexOf("await ShutdownNativeSessionAsync(", complete, StringComparison.Ordinal);
        Assert.True(complete >= 0);
        Assert.True(completeShutdown > complete);

        var failedStart = source.IndexOf("async Task ShutdownNativeAfterFailedStartAsync", StringComparison.Ordinal);
        var failedStartShutdown = source.IndexOf("await ShutdownNativeSessionAsync(", failedStart, StringComparison.Ordinal);
        Assert.True(failedStart >= 0);
        Assert.True(failedStartShutdown > failedStart);

        var shutdownHelper = source.IndexOf(
            "async Task<Exception?> ShutdownNativeSessionAsync",
            StringComparison.Ordinal);
        var nativeShutdown = source.IndexOf(
            "await protocolClient.ShutdownAsync().ConfigureAwait(false);",
            shutdownHelper,
            StringComparison.Ordinal);
        var shutdownEnd = source.IndexOf(
            "async Task<string[]?> PullMessagesAsync",
            shutdownHelper,
            StringComparison.Ordinal);
        Assert.True(shutdownHelper >= 0);
        Assert.True(nativeShutdown > shutdownHelper);
        Assert.True(shutdownEnd > nativeShutdown);
        Assert.DoesNotContain(
            "Task.Run(",
            source[shutdownHelper..shutdownEnd],
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "GetAwaiter().GetResult()",
            source[shutdownHelper..shutdownEnd],
            StringComparison.Ordinal);
    }

    [Fact]
    public void MacHostBinding_IsQueuedOffTheUiThreadAndTrackedPerGeneration()
    {
        var source = ReadRepositoryFile("Editor", "Services", "EngineService.cs");
        var lifecycleSource = ReadRepositoryFile("Editor", "Scene", "SceneViewportLifecycle.cs");

        Assert.Contains("Dictionary<ulong, (long Generation, nint Handle)> appliedMacRemoteViewportHosts", source, StringComparison.Ordinal);

        var bind = source.IndexOf("public void BindMacRemoteViewportHost", StringComparison.Ordinal);
        var generation = source.IndexOf("Volatile.Read(ref engineGeneration)", bind, StringComparison.Ordinal);
        var runningGuard = source.IndexOf("if (!IsGenerationActive(generation))", generation, StringComparison.Ordinal);
        var removeWhileStopped = source.IndexOf("appliedMacRemoteViewportHosts.Remove(viewportId);", runningGuard, StringComparison.Ordinal);
        var acknowledge = source.IndexOf("appliedMacRemoteViewportHosts[viewportId] =", removeWhileStopped, StringComparison.Ordinal);
        var queuedInterop = source.IndexOf(
            "QueuePlatformInterop(async cancellationToken =>",
            acknowledge,
            StringComparison.Ordinal);
        var nativeBind = source.IndexOf(
            "await protocolClient.SetRemoteViewportMacHostHandleAsync(",
            queuedInterop,
            StringComparison.Ordinal);
        var removeAfterFailure = source.IndexOf("appliedMacRemoteViewportHosts.Remove(viewportId);", nativeBind, StringComparison.Ordinal);
        Assert.True(bind >= 0);
        Assert.True(generation > bind);
        Assert.True(runningGuard > generation);
        Assert.True(removeWhileStopped > runningGuard);
        Assert.True(acknowledge > removeWhileStopped);
        Assert.True(queuedInterop > acknowledge);
        Assert.True(nativeBind > queuedInterop);
        Assert.True(removeAfterFailure > nativeBind);

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
    public void SuccessfulLocalWorldMutation_AdvancesSnapshotGateAfterAwaitedInterop()
    {
        var source = ReadRepositoryFile("Editor", "Services", "EngineService.cs");

        var helper = source.IndexOf(
            "async Task<bool> InvokeRunningInteropAsync",
            StringComparison.Ordinal);
        var nativeAction = source.IndexOf(
            "!await action(cancellationToken).ConfigureAwait(false)",
            helper,
            StringComparison.Ordinal);
        var reserveSequence = source.IndexOf(
            "worldSnapshotPublication.ReserveSequence()",
            nativeAction,
            StringComparison.Ordinal);
        var advanceGate = source.IndexOf(
            "worldSnapshotPublication.TryAdvance(mutationSequence)",
            reserveSequence,
            StringComparison.Ordinal);
        var helperEnd = source.IndexOf("public static void ShowMainWindow", advanceGate, StringComparison.Ordinal);
        var commitChanges = source.IndexOf(
            "public Task<bool> CommitChangesAsync",
            helperEnd,
            StringComparison.Ordinal);
        var invalidateQueuedSnapshots = source.IndexOf(
            "invalidateQueuedWorldSnapshots: true",
            commitChanges,
            StringComparison.Ordinal);

        Assert.True(helper >= 0);
        Assert.True(nativeAction > helper);
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
        var stateLock = source.IndexOf("lock (sceneViewportStateLock)", update, StringComparison.Ordinal);
        var rememberState = source.IndexOf("sceneViewportState.Remember(rect, visible, focused)", stateLock, StringComparison.Ordinal);
        var refresh = source.IndexOf(
            "async Task<bool> TryRefreshSceneRemoteViewportAsync",
            rememberState,
            StringComparison.Ordinal);
        var refreshStateLock = source.IndexOf("lock (sceneViewportStateLock)", refresh, StringComparison.Ordinal);
        var captureState = source.IndexOf("viewportState = sceneViewportState.Capture();", refreshStateLock, StringComparison.Ordinal);
        var preservedRect = source.IndexOf("viewportState.Rect,", captureState, StringComparison.Ordinal);
        var preservedVisibility = source.IndexOf("viewportState.Visible,", preservedRect, StringComparison.Ordinal);
        var preservedFocus = source.IndexOf("viewportState.Focused", preservedVisibility, StringComparison.Ordinal);
        var bootstrap = source.IndexOf(
            "pollTasks.Add(RunPeriodicTaskAsync(async () =>",
            rememberState,
            StringComparison.Ordinal);
        var remoteUpdate = source.IndexOf(
            "await TryRefreshSceneRemoteViewportAsync(",
            bootstrap,
            StringComparison.Ordinal);

        Assert.True(update >= 0);
        Assert.True(stateLock > update);
        Assert.True(rememberState > stateLock);
        Assert.True(refresh > rememberState);
        Assert.True(refreshStateLock > refresh);
        Assert.True(captureState > refreshStateLock);
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
    public void NativeExitCode_IsRoutedThroughTheWebSocketProtocolGateway()
    {
        var managedSource = ReadRepositoryFile("Editor", "Services", "EngineService.cs");
        var clientSource = ReadRepositoryFile("Editor", "Protocol", "EngineProtocolClient.cs");
        var transportSource = ReadRepositoryFile(
            "Editor",
            "Protocol",
            "ClientWebSocketEngineProtocolTransport.cs");
        var serverSource = ReadRepositoryFile(
            "Lib",
            "EditorEngineWebSocketServer.cpp");
        var dispatcherSource = ReadRepositoryFile("Lib", "EditorEngineProtocol.cpp");

        Assert.Contains(
            "=> protocolClient.GetExitCodeAsync(cancellationToken);",
            managedSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "public async Task<int> GetExitCodeAsync(",
            clientSource,
            StringComparison.Ordinal);
        Assert.Contains("GetExitCode = new Empty()", clientSource, StringComparison.Ordinal);
        Assert.Contains("WebSocketMessageType.Binary", transportSource, StringComparison.Ordinal);
        Assert.Contains("InvokeEditorEngineProtocol(", serverSource, StringComparison.Ordinal);
        Assert.Contains("case ProtocolRequest::kGetExitCode:", dispatcherSource, StringComparison.Ordinal);
        Assert.Contains("Sailor::App::GetExitCode()", dispatcherSource, StringComparison.Ordinal);
    }

    [Fact]
    public void EngineTypeSerialization_IsRoutedThroughTheSharedProtocolAbi()
    {
        var schemaSource = ReadRepositoryFile("Protocol", "editor_engine.proto");
        var clientSource = ReadRepositoryFile("Editor", "Protocol", "EngineProtocolClient.cs");
        var exportSource = ReadRepositoryFile("Lib", "EditorProtocolExports.cpp");
        var dispatcherSource = ReadRepositoryFile("Lib", "EditorEngineProtocol.cpp");
        var workspaceDocumentation = ReadRepositoryFile("Docs", "WorkspaceLogic.md");

        Assert.Contains("Empty serialize_engine_types = 46;", schemaSource, StringComparison.Ordinal);
        Assert.Contains(
            "public async Task<string> SerializeEngineTypesAsync(",
            clientSource,
            StringComparison.Ordinal);
        Assert.Contains("SerializeEngineTypes = new Empty()", clientSource, StringComparison.Ordinal);
        Assert.Contains("case ProtocolRequest::kSerializeEngineTypes:", dispatcherSource, StringComparison.Ordinal);
        Assert.Contains("Sailor::App::SerializeEngineTypes(value.GetOutput())", dispatcherSource, StringComparison.Ordinal);
        Assert.DoesNotContain("SerializeEngineTypes", exportSource, StringComparison.Ordinal);
        Assert.Contains(
            "Both commands use the shared authenticated binary WebSocket transport",
            workspaceDocumentation,
            StringComparison.Ordinal);
    }

    [Fact]
    public void CreateEditorWorld_IsRoutedThroughTheSharedProtocolAbi()
    {
        var managedSource = ReadRepositoryFile("Editor", "Services", "EngineService.cs");
        var clientSource = ReadRepositoryFile("Editor", "Protocol", "EngineProtocolClient.cs");
        var dispatcherSource = ReadRepositoryFile("Lib", "EditorEngineProtocol.cpp");

        Assert.Contains(
            "await protocolClient.CreateEditorWorldAsync(",
            managedSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "public async Task<bool> CreateEditorWorldAsync(",
            clientSource,
            StringComparison.Ordinal);
        Assert.Contains("CreateEditorWorld = new Empty()", clientSource, StringComparison.Ordinal);
        Assert.Contains("case ProtocolRequest::kCreateEditorWorld:", dispatcherSource, StringComparison.Ordinal);
        Assert.Contains("Sailor::App::CreateEditorWorld()", dispatcherSource, StringComparison.Ordinal);
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
        var protocolClientSource = ReadRepositoryFile(
            "Editor",
            "Protocol",
            "EngineProtocolClient.cs");
        var protocolDispatcherSource = ReadRepositoryFile(
            "Lib",
            "EditorEngineProtocol.cpp");
        var schedulerHeader = ReadRepositoryFile("Runtime", "Tasks", "Scheduler.h");
        var schedulerSource = ReadRepositoryFile("Runtime", "Tasks", "Scheduler.cpp");

        Assert.Contains(
            "!await protocolClient.RequestAssetReloadAsync(",
            managedSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "RequestAssetReload = new Empty()",
            protocolClientSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "case ProtocolRequest::kRequestAssetReload:",
            protocolDispatcherSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "SetBoolResult(response, Sailor::App::RequestAssetReload());",
            protocolDispatcherSource,
            StringComparison.Ordinal);

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
        var protocolClientSource = ReadRepositoryFile(
            "Editor",
            "Protocol",
            "EngineProtocolClient.cs");
        var protocolDispatcherSource = ReadRepositoryFile(
            "Lib",
            "EditorEngineProtocol.cpp");

        Assert.Contains(
            "await protocolClient.GetAssetReloadStateAsync(",
            managedSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "public async Task<EngineProtocolAssetReloadState> GetAssetReloadStateAsync(",
            protocolClientSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "case ProtocolRequest::kGetAssetReloadState:",
            protocolDispatcherSource,
            StringComparison.Ordinal);
        Assert.Contains("public event Action<AssetReloadCompletion> OnAssetReloadCompleted", managedSource, StringComparison.Ordinal);
        Assert.Contains("PublishAssetReloadCompletion(", managedSource, StringComparison.Ordinal);
        Assert.Contains(
            "reloadState.SuccessfulGeneration ==",
            managedSource,
            StringComparison.Ordinal);

        var requestReloadStart = managedSource.IndexOf(
            "public async Task<bool> RequestAssetReloadAsync",
            StringComparison.Ordinal);
        var requestReloadEnd = managedSource.IndexOf(
            "public async Task RefreshCurrentWorldAsync",
            requestReloadStart,
            StringComparison.Ordinal);
        Assert.True(requestReloadStart >= 0);
        Assert.True(requestReloadEnd > requestReloadStart);
        var requestReloadBody =
            managedSource[requestReloadStart..requestReloadEnd];
        const string stateRead =
            "await protocolClient.GetAssetReloadStateAsync(";
        var initialStateRead = requestReloadBody.IndexOf(
            stateRead,
            StringComparison.Ordinal);
        var publishTarget = requestReloadBody.IndexOf(
            "ref targetReloadGeneration,",
            initialStateRead,
            StringComparison.Ordinal);
        var postPublicationStateRead = requestReloadBody.IndexOf(
            stateRead,
            initialStateRead + stateRead.Length,
            StringComparison.Ordinal);
        var waitForEvent = requestReloadBody.IndexOf(
            "return await completionSource.Task",
            postPublicationStateRead,
            StringComparison.Ordinal);
        Assert.True(initialStateRead >= 0);
        Assert.True(publishTarget > initialStateRead);
        Assert.True(postPublicationStateRead > publishTarget);
        Assert.True(waitForEvent > postPublicationStateRead);
        Assert.Equal(
            -1,
            requestReloadBody.IndexOf(
                stateRead,
                postPublicationStateRead + stateRead.Length,
                StringComparison.Ordinal));

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
    public void ExistingAssetSave_UsesTargetedUpdateWithoutScanningContent()
    {
        var controlPanelSource = ReadRepositoryFile(
            "Editor",
            "Views",
            "InspectorView",
            "ControlPanelView.xaml.cs");
        var assetsSource = ReadRepositoryFile(
            "Editor",
            "Services",
            "AssetsService.cs");
        var worldSource = ReadRepositoryFile(
            "Editor",
            "Services",
            "WorldService.cs");
        var engineServiceSource = ReadRepositoryFile(
            "Editor",
            "Services",
            "EngineService.cs");
        var protocolClientSource = ReadRepositoryFile(
            "Editor",
            "Protocol",
            "EngineProtocolClient.cs");
        var protocolDispatcherSource = ReadRepositoryFile(
            "Lib",
            "EditorEngineProtocol.cpp");
        var nativeSource = ReadRepositoryFile("Runtime", "Sailor.cpp");

        Assert.Contains(
            "assetsService.SaveAssetAsync(assetFile)",
            controlPanelSource,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "RequestAssetReloadAsync",
            controlPanelSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "public async Task<bool> SaveExistingAssetAsync(",
            assetsSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "return await _engineService.UpdateAssetAsync(",
            assetsSource,
            StringComparison.Ordinal);
        var existingWorldSaveStart = worldSource.IndexOf(
            "async Task<SceneSaveResult> SaveExistingWorldAsync(",
            StringComparison.Ordinal);
        var existingWorldSaveEnd = worldSource.IndexOf(
            "async Task<SceneSaveResult> SaveCurrentWorldAsAsync(",
            existingWorldSaveStart,
            StringComparison.Ordinal);
        Assert.True(existingWorldSaveStart >= 0);
        Assert.True(existingWorldSaveEnd > existingWorldSaveStart);
        Assert.Contains(
            "assetsService.SaveExistingAssetAsync(",
            worldSource[existingWorldSaveStart..existingWorldSaveEnd],
            StringComparison.Ordinal);
        Assert.Contains(
            "protocolClient.UpdateAssetAsync(stringId, token)",
            engineServiceSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "UpdateAsset = new FileIdRequest",
            protocolClientSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "case ProtocolRequest::kUpdateAsset:",
            protocolDispatcherSource,
            StringComparison.Ordinal);

        var updateAssetStart = nativeSource.IndexOf(
            "bool App::UpdateAsset(",
            StringComparison.Ordinal);
        var updateAssetEnd = nativeSource.IndexOf(
            "\nbool App::",
            updateAssetStart + 1,
            StringComparison.Ordinal);
        Assert.True(updateAssetStart >= 0);
        Assert.True(updateAssetEnd > updateAssetStart);
        var updateAssetBody = nativeSource[updateAssetStart..updateAssetEnd];
        Assert.DoesNotContain("ScanContentFolder", updateAssetBody, StringComparison.Ordinal);
        Assert.DoesNotContain("WaitIdle", updateAssetBody, StringComparison.Ordinal);
    }

    [Fact]
    public void AssetProjection_TreatsGlslAsShaderLibraryDespiteSharedNativeAssetInfoType()
    {
        var assetsSource = ReadRepositoryFile("Editor", "Services", "AssetsService.cs");
        var reflectionSource = ReadRepositoryFile("Runtime", "Core", "Reflection.cpp");

        Assert.Contains(
            "\"Sailor::ShaderAssetInfo\" when string.Equals(",
            assetsSource,
            StringComparison.Ordinal);
        Assert.Contains("\".glsl\"", assetsSource, StringComparison.Ordinal);
        Assert.Contains("=> new ShaderLibraryFile()", assetsSource, StringComparison.Ordinal);
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
    public void AssetProjection_SkipsInternalTransactionDirectories()
    {
        var assetsSource = ReadRepositoryFile("Editor", "Services", "AssetsService.cs");

        Assert.Contains(
            "ProjectContentInternalPathPolicy.IsTransactionDirectory(directory)",
            assetsSource,
            StringComparison.Ordinal);
    }

    [Fact]
    public void AssetProjection_LoadsFoldersAndMetadataLazilyOffTheUiThread()
    {
        var assetsSource = ReadRepositoryFile("Editor", "Services", "AssetsService.cs");
        var assetFileSource = ReadRepositoryFile("Editor", "ViewModels", "AssetFile.cs");
        var selectionSource = ReadRepositoryFile("Editor", "Services", "SelectionService.cs");
        var contentViewSource = ReadRepositoryFile("Editor", "Views", "ContentFolderView.xaml.cs");
        var cacheIndexSource = ReadRepositoryFile("Editor", "Services", "AssetCacheIndexStore.cs");

        var addRootStart = assetsSource.IndexOf(
            "public void AddProjectRoot(EngineLaunchContext launchContext)",
            StringComparison.Ordinal);
        var watcherStart = assetsSource.IndexOf(
            "void ConfigureContentWatchers",
            addRootStart,
            StringComparison.Ordinal);
        Assert.True(addRootStart >= 0 && watcherStart > addRootStart);
        var addRootBody = assetsSource[addRootStart..watcherStart];
        Assert.Contains("AddContentRootFolder(", addRootBody, StringComparison.Ordinal);
        Assert.DoesNotContain("ReadDirectory(", addRootBody, StringComparison.Ordinal);
        Assert.DoesNotContain("ReadAssetFile(", addRootBody, StringComparison.Ordinal);

        Assert.Contains("public async Task EnsureFolderLoadedAsync(", assetsSource, StringComparison.Ordinal);
        Assert.Contains("() => ReadDirectoryLevel(folder)", assetsSource, StringComparison.Ordinal);
        Assert.Contains("await Task.Run(", assetsSource, StringComparison.Ordinal);
        Assert.Contains("IsLoaded = false", assetsSource, StringComparison.Ordinal);
        Assert.Contains("await service.EnsureFolderLoadedAsync(folderId)", contentViewSource, StringComparison.Ordinal);
        Assert.Contains("public async Task EnsureMetadataLoadedAsync(", assetFileSource, StringComparison.Ordinal);
        Assert.Contains("await Task.Run(Revert, cancellationToken)", assetFileSource, StringComparison.Ordinal);
        Assert.Contains("await selectedAssetFile.EnsureMetadataLoadedAsync(", selectionSource, StringComparison.Ordinal);
        Assert.Contains("QueueAssetCacheIndexLoad(launchContext, contentGeneration)", assetsSource, StringComparison.Ordinal);
        Assert.Contains("MergeAssetCacheIndex(result.Entries!)", assetsSource, StringComparison.Ordinal);
        Assert.Contains("Files.Add(file)", assetsSource, StringComparison.Ordinal);
        Assert.Contains("Assets[file.FileId] = file", assetsSource, StringComparison.Ordinal);
        Assert.Contains("producerIdentity", cacheIndexSource, StringComparison.Ordinal);
        Assert.Contains("asset-cache-v1", cacheIndexSource, StringComparison.Ordinal);
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
    public void WorkspaceCacheIdentity_IsReturnedByTheTypedProtocolClient()
    {
        var managedSource = ReadRepositoryFile("Editor", "Services", "EngineService.cs");
        var clientSource = ReadRepositoryFile("Editor", "Protocol", "EngineProtocolClient.cs");
        var dispatcherSource = ReadRepositoryFile("Lib", "EditorEngineProtocol.cpp");

        Assert.Contains(
            "await protocolClient.SerializeWorkspaceCacheIdentityAsync(",
            managedSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "SerializeWorkspaceCacheIdentity = new Empty()",
            clientSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "case ProtocolRequest::kSerializeWorkspaceCacheIdentity:",
            dispatcherSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "DispatchSerializeWorkspaceCacheIdentity(response);",
            dispatcherSource,
            StringComparison.Ordinal);
        Assert.DoesNotContain("Marshal.PtrToString", managedSource, StringComparison.Ordinal);
    }

    [Fact]
    public void ManagedInitialization_UsesTypedRepeatedUtf8ProtocolArguments()
    {
        var managedSource = ReadRepositoryFile("Editor", "Services", "EngineService.cs");
        var nativeInteropSource = ReadRepositoryFile(
            "Editor",
            "Protocol",
            "EngineProtocolNative.cs");
        var clientSource = ReadRepositoryFile(
            "Editor",
            "Protocol",
            "EngineProtocolClient.cs");
        var dispatcherSource = ReadRepositoryFile("Lib", "EditorEngineProtocol.cpp");
        var nativeSource = ReadRepositoryFile("Runtime", "Sailor.cpp");

        Assert.Contains(
            "() => protocolClient.InitializeAsync(",
            managedSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "initialize.Arguments.Add(ValidateString(argument, nameof(arguments)));",
            clientSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "normalizedValue.Contains('\\0', StringComparison.Ordinal)",
            clientSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "Initialize = initialize",
            clientSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "EntryPoint = \"SailorProtocolStartLocalHost\"",
            nativeInteropSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "EntryPoint = \"SailorProtocolRequestLocalHostStop\"",
            nativeInteropSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "EntryPoint = \"SailorProtocolStopLocalHost\"",
            nativeInteropSource,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "SailorProtocolInvoke",
            nativeInteropSource,
            StringComparison.Ordinal);
        Assert.DoesNotContain(
            "SailorProtocolFreeBuffer",
            nativeInteropSource,
            StringComparison.Ordinal);
        Assert.Equal(
            3,
            nativeInteropSource.Split("[DllImport(", StringSplitOptions.None).Length - 1);
        Assert.Contains(
            "const int numArguments = request.arguments_size();",
            dispatcherSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "arguments[i] = request.arguments(i).c_str();",
            dispatcherSource,
            StringComparison.Ordinal);
        Assert.Contains(
            "Sailor::App::Initialize(arguments.GetRawPtr(), numArguments);",
            dispatcherSource,
            StringComparison.Ordinal);

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

        var initialize = source.IndexOf(
            "() => protocolClient.InitializeAsync(",
            StringComparison.Ordinal);
        var identity = source.IndexOf(
            "await ReadWorkspaceCacheIdentityAsync(",
            initialize,
            StringComparison.Ordinal);
        var cacheLoad = source.IndexOf("editorTypeCacheStore.Load(", identity, StringComparison.Ordinal);
        var cachedValidation = source.IndexOf("TryParseEditorTypes(", cacheLoad, StringComparison.Ordinal);
        var liveCatalog = source.IndexOf(
            "string serializedEditorTypes =",
            cacheLoad,
            StringComparison.Ordinal);
        var liveCatalogRead = source.IndexOf(
            "await SerializeEditorTypesAsync(",
            liveCatalog,
            StringComparison.Ordinal);
        var liveValidation = source.IndexOf("TryParseEditorTypes(", liveCatalog, StringComparison.Ordinal);
        var livePublication = source.IndexOf("Volatile.Write(ref editorTypes, liveEditorTypes)", liveValidation, StringComparison.Ordinal);
        var cacheSave = source.IndexOf("editorTypeCacheStore.Save(", liveCatalog, StringComparison.Ordinal);

        Assert.True(initialize >= 0);
        Assert.True(identity > initialize);
        Assert.True(cacheLoad > identity);
        Assert.True(cachedValidation > cacheLoad);
        Assert.True(liveCatalog > cachedValidation);
        Assert.True(liveCatalogRead > liveCatalog);
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
    public void StartupWaitsForTheEngineMainThreadBeforeBootstrapSerialization()
    {
        var source = ReadRepositoryFile(
            "Editor",
            "Services",
            "EngineService.cs").ReplaceLineEndings("\n");

        var nativeStart = source.IndexOf(
            "await protocolClient.StartAsync(",
            source.IndexOf("initialized = true;", StringComparison.Ordinal),
            StringComparison.Ordinal);
        var waitUntilReady = source.IndexOf(
            "await WaitForEngineMainThreadAsync(",
            nativeStart,
            StringComparison.Ordinal);
        var startMonitor = source.IndexOf(
            "runtimeMonitorTask = MonitorEngineLifetimeAsync(",
            waitUntilReady,
            StringComparison.Ordinal);
        var startupOrder = source.IndexOf(
            "// Required startup order: combined editor catalog, world, then initial messages.",
            startMonitor,
            StringComparison.Ordinal);
        var liveCatalog = source.IndexOf(
            "string serializedEditorTypes =",
            startupOrder,
            StringComparison.Ordinal);
        var liveCatalogRead = source.IndexOf(
            "await SerializeEditorTypesAsync(",
            liveCatalog,
            StringComparison.Ordinal);
        var macBranch = source.IndexOf("#if MACCATALYST", liveCatalog, StringComparison.Ordinal);
        var mainThreadDispatch = source.IndexOf(
            "await MainThread.InvokeOnMainThreadAsync(",
            macBranch,
            StringComparison.Ordinal);
        var worldSerialization = source.IndexOf(
            "() => SerializeWorldAsync(",
            mainThreadDispatch,
            StringComparison.Ordinal);

        Assert.True(nativeStart >= 0);
        Assert.True(waitUntilReady > nativeStart);
        Assert.True(startMonitor > waitUntilReady);
        Assert.True(startupOrder >= 0);
        Assert.True(startupOrder > startMonitor);
        Assert.True(liveCatalog > startupOrder);
        Assert.True(liveCatalogRead > liveCatalog);
        Assert.True(macBranch > liveCatalog);
        Assert.True(mainThreadDispatch > macBranch);
        Assert.True(worldSerialization > mainThreadDispatch);
        Assert.Contains(
            "protocolClient.IsEngineMainThreadReadyAsync(\n                        cancellationToken)",
            source,
            StringComparison.Ordinal);
        Assert.Contains(
            "protocolClient.IsEngineRunningAsync(\n                        cancellationToken)",
            source,
            StringComparison.Ordinal);
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
