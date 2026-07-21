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
        var clearReusable = source.IndexOf(
            "sReusableEditorRenderingWindow = nil;",
            acquireReusable,
            StringComparison.Ordinal);
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
        var preserveReusable = source.IndexOf(
            "sReusableEditorRenderingWindow = window;",
            orderOut,
            StringComparison.Ordinal);
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
        Assert.DoesNotContain(
            "[window release];",
            source[orderOut..nativeCloseHandler],
            StringComparison.Ordinal);
        Assert.Contains("delegate.terminateApplicationOnClose = !bRunsInsideEditor;", source, StringComparison.Ordinal);
        Assert.Contains("if (bTerminateApplicationOnClose)", source, StringComparison.Ordinal);
    }

    [Fact]
    public void Win32NativeWindow_TeardownOwnsClassAndDetachesBeforeDestroyCallbacks()
    {
        var header = ReadRepositoryFile("Runtime", "Platform", "Win32", "Window.h");
        var source = ReadRepositoryFile("Runtime", "Platform", "Win32", "Window.cpp");

        Assert.Contains("ATOM m_windowClassAtom = 0;", header, StringComparison.Ordinal);
        Assert.Contains("m_windowClassAtom = RegisterClassEx(&wcx);", source, StringComparison.Ordinal);

        var destructor = source.IndexOf("Window::~Window()", StringComparison.Ordinal);
        var destructorCleanup = source.IndexOf("Destroy();", destructor, StringComparison.Ordinal);
        var destroy = source.IndexOf("void Window::Destroy()", StringComparison.Ordinal);
        var ownerThreadCheck = source.IndexOf("GetWindowThreadProcessId", destroy, StringComparison.Ordinal);
        var clearHandle = source.IndexOf("m_hWnd = nullptr;", destroy, StringComparison.Ordinal);
        var unregisterWindow = source.IndexOf("g_windows.Remove(this);", destroy, StringComparison.Ordinal);
        var nativeDestroy = source.IndexOf("DestroyWindow(hWnd)", destroy, StringComparison.Ordinal);
        var unregisterClass = source.IndexOf("UnregisterClass(MAKEINTATOM(windowClassAtom)", nativeDestroy, StringComparison.Ordinal);
        Assert.True(destructor >= 0);
        Assert.True(destructorCleanup > destructor);
        Assert.True(destroy >= 0);
        Assert.True(ownerThreadCheck > destroy);
        Assert.True(clearHandle > ownerThreadCheck);
        Assert.True(unregisterWindow > clearHandle);
        Assert.True(nativeDestroy > unregisterWindow);
        Assert.True(unregisterClass > nativeDestroy);

        var windowProc = source.IndexOf("LRESULT CALLBACK Sailor::Win32::WindowProc", StringComparison.Ordinal);
        var nullGuard = source.IndexOf("if (!pWindow)", windowProc, StringComparison.Ordinal);
        var defaultDispatch = source.IndexOf("return DefWindowProc(hWnd, msg, wParam, lParam);", nullGuard, StringComparison.Ordinal);
        Assert.True(windowProc >= 0);
        Assert.True(nullGuard > windowProc);
        Assert.True(defaultDispatch > nullGuard);
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
