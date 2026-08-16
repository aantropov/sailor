using SailorEngine;
using System.Diagnostics;
using SailorEditor.Protocol;
using SailorEditor.Protocol.Generated;
using SailorEditor.Utility;
using SailorEditor.Workspace;
using SailorEditor.Scene;
using System.Threading;
using System.Globalization;

namespace SailorEditor.Services
{
    public enum EngineLifecycleState
    {
        Stopped,
        Starting,
        Running,
        Stopping,
        Faulted
    }

    public sealed class EngineLifecycleException : Exception
    {
        public EngineLifecycleException(string message, int exitCode, Exception? innerException = null)
            : base(message, innerException)
        {
            ExitCode = exitCode;
        }

        public int ExitCode { get; }
    }

    public readonly record struct AssetReloadCompletion(ulong Generation, bool Succeeded);

    enum EditorManagedMutationKind : uint
    {
        Selection = 1,
        ObjectTransform = 2,
    }

    public enum RemoteViewportInputKind : uint
    {
        Unknown = 0,
        PointerMove = 1,
        PointerButton = 2,
        PointerWheel = 3,
        Key = 4,
        Focus = 5,
        Capture = 6
    }

    [Flags]
    public enum RemoteViewportInputModifier : uint
    {
        None = 0,
        Shift = 1 << 0,
        Control = 1 << 1,
        Alt = 1 << 2,
        Meta = 1 << 3,
        MouseLeft = 1 << 4,
        MouseRight = 1 << 5,
        MouseMiddle = 1 << 6
    }

    public enum RemoteViewportSessionState : uint
    {
        Created = 0,
        Negotiating = 1,
        Ready = 2,
        Active = 3,
        Paused = 4,
        Resizing = 5,
        Recovering = 6,
        Lost = 7,
        Terminating = 8,
        Disposed = 9
    }

    internal class EngineService : IDisposable, IAsyncDisposable
    {
        sealed class EngineSession
        {
            public required long Generation { get; init; }
            public required EngineLaunchContext LaunchContext { get; init; }
            public required CancellationTokenSource PollCancellation { get; init; }
            public required CancellationTokenSource BackgroundCancellation { get; init; }
            public required CancellationTokenSource RuntimeMonitorCancellation { get; init; }
            public required Task RuntimeMonitorTask { get; init; }
            public required IReadOnlyList<Task> PollTasks { get; init; }
            public Task CompletionTask { get; set; } = Task.CompletedTask;
        }

        readonly record struct ConsoleMessage(long Generation, string Text);
        readonly record struct RemoteViewportUpdate(
            ulong ViewportId,
            Rect Rect,
            bool Visible,
            bool Focused);
        readonly record struct RemoteViewportInput(
            ulong ViewportId,
            RemoteViewportInputKind Kind,
            float PointerX,
            float PointerY,
            float WheelDeltaX,
            float WheelDeltaY,
            uint KeyCode,
            uint Button,
            RemoteViewportInputModifier Modifiers,
            bool Pressed,
            bool Focused,
            bool Captured);

        public const ulong SceneViewportId = 1;

        readonly object platformInteropQueueLock = new();
        readonly object sceneViewportStateLock = new();
        readonly object runLock = new();
        readonly object disposeGate = new();
        readonly EngineProtocolClient protocolClient;
        readonly SemaphoreSlim lifecycleGate = new(1, 1);
        readonly SemaphoreSlim restartGate = new(1, 1);
        readonly CancellationTokenSource disposeCancellation = new();
        readonly RingBufferedBatcher<ConsoleMessage> consoleMessages = new(MaxBufferedConsoleMessages);
        readonly WorldSnapshotPublicationGate worldSnapshotPublication = new();
        readonly EditorViewportEventEpochGate editorViewportEventEpoch = new();
        readonly LatestSceneViewportState<Rect> sceneViewportState = new(new Rect(0, 0, 1024, 768));
        Task platformInteropQueue = Task.CompletedTask;
        int pendingPlatformInteropCommands;
        Task? disposeTask;
        EngineTypes editorTypes = new();
        int consoleDispatchScheduled = 0;
        int disposeState;
        int lifecycleState = (int)EngineLifecycleState.Stopped;
        long engineGeneration;
#if WINDOWS || MACCATALYST
        readonly object remoteViewportStateLock = new();
        readonly Dictionary<ulong, RemoteViewportSessionState> remoteViewportStates = [];
        readonly Dictionary<ulong, string> remoteViewportDiagnostics = [];
        readonly KeyedLatestQueuedCommand<ulong, RemoteViewportUpdate> remoteViewportUpdates = new();
        readonly LatestQueuedCommand<Rect> editorViewportUpdate = new();
        readonly LatestQueuedCommand<(uint Width, uint Height)> renderTargetUpdate = new();
        readonly KeyedLatestQueuedCommand<ulong, RemoteViewportInput> pointerMoves = new();
        readonly KeyedLatestQueuedCommand<ulong, ulong> viewportStatusRefreshes = new();
#endif
#if MACCATALYST
        readonly object macRemoteViewportHostLock = new();
        readonly Dictionary<ulong, (long Generation, nint Handle)> appliedMacRemoteViewportHosts = [];
#endif
#if WINDOWS
        int windowsViewportInteropFailureLogged;
#endif
        EngineSession? activeSession;
        EngineLaunchContext? activeLaunchContext;
        int lastExitCode;
        Exception? lastFailure;
        static EngineService? currentInstance;
        const int MaxBufferedConsoleMessages = 1000;
        const int MaxEditorViewportEventsPerPoll = 64;
        const int MaxPendingPlatformInteropCommands = 128;
        static readonly TimeSpan LifecycleProbeTimeout =
            TimeSpan.FromSeconds(5);
        static readonly TimeSpan RuntimeLivenessPollInterval =
            TimeSpan.FromMilliseconds(250);
        static readonly TimeSpan DisposeOrderlyStopTimeout =
            TimeSpan.FromSeconds(5);
        static readonly TimeSpan DisposeCompletionTimeout =
            TimeSpan.FromSeconds(5);
        static readonly TimeSpan DisposeAbortDrainTimeout =
            TimeSpan.FromSeconds(2);
        static readonly TimeSpan DisposePlatformQueueTimeout =
            TimeSpan.FromSeconds(2);

        readonly string repoRoot = ResolveRepoRoot();
        readonly WorkspaceLifecycleService workspaceLifecycle;
        readonly EditorTypeCacheStore editorTypeCacheStore = new();

        public EngineService(WorkspaceLifecycleService workspaceLifecycle)
            : this(workspaceLifecycle, new EngineProtocolClient())
        {
        }

        internal EngineService(
            WorkspaceLifecycleService workspaceLifecycle,
            EngineProtocolClient protocolClient)
        {
            this.workspaceLifecycle = workspaceLifecycle ??
                throw new ArgumentNullException(nameof(workspaceLifecycle));
            this.protocolClient = protocolClient ??
                throw new ArgumentNullException(nameof(protocolClient));
            Volatile.Write(ref currentInstance, this);
        }

        public EngineLifecycleState State => (EngineLifecycleState)Volatile.Read(ref lifecycleState);
        public bool IsRunning => State == EngineLifecycleState.Running;
        public int LastExitCode => Volatile.Read(ref lastExitCode);

        public Exception? LastFailure
        {
            get
            {
                lock (runLock)
                {
                    return lastFailure;
                }
            }
        }

        public EngineLaunchContext? ActiveLaunchContext
        {
            get
            {
                lock (runLock)
                {
                    return activeLaunchContext;
                }
            }
        }

        public Task RuntimeMonitorTask
        {
            get
            {
                lock (runLock)
                {
                    return activeSession?.RuntimeMonitorTask ?? Task.CompletedTask;
                }
            }
        }

        public event Action<EngineLifecycleState> OnLifecycleStateChanged = delegate { };
        public event Action<AssetReloadCompletion> OnAssetReloadCompleted = delegate { };
        public event Action<IReadOnlyList<EditorViewportEvent>> OnEditorViewportEvents = delegate { };
        public event Action<bool> OnEditorSimulationStateChanged = delegate { };

        public string EngineContentDirectory => Path.Combine(repoRoot, "Content");

#if MACCATALYST
        public string EngineCacheDirectory
        {
            get
            {
                var localAppData = Microsoft.Maui.Storage.FileSystem.Current.AppDataDirectory;
                return Path.Combine(localAppData, "SailorEditor", "Cache");
            }
        }
#else
        public string EngineCacheDirectory => Path.Combine(repoRoot, "Cache");
#endif

        public string EngineWorkingDirectory => repoRoot + Path.DirectorySeparatorChar;

        public string EditorTypesCacheFilePath => GetLaunchContext().EditorTypesCacheFilePath;

        public EngineLaunchContext GetLaunchContext()
        {
            var workspace = workspaceLifecycle.Current;
            return EngineLaunchContract.Resolve(
                workspace?.WorkspaceRoot,
                workspace?.ManifestPath,
                workspace?.ContentDirectory,
                workspace?.CacheDirectory,
                EngineWorkingDirectory,
                workspace?.Manifest.WorkspaceId);
        }

        public string PathToEngineExecDebug
        {
            get
            {
#if WINDOWS
                return Path.Combine(EngineWorkingDirectory, "SailorEngine-Debug.exe");
#elif MACCATALYST
                return Path.Combine(
                    EngineWorkingDirectory,
                    "Binaries",
                    "Debug",
                    "SailorEngine-Debug");
#else
                return Path.Combine(EngineWorkingDirectory, "Binaries", "SailorEngine-Debug");
#endif
            }
        }

        public string PathToEngineExec
        {
            get
            {
#if WINDOWS
                return Path.Combine(EngineWorkingDirectory, "SailorEngine-Release.exe");
#elif MACCATALYST
                return Path.Combine(
                    EngineWorkingDirectory,
                    "Binaries",
                    "Release",
                    "SailorEngine-Release");
#else
                return Path.Combine(EngineWorkingDirectory, "Binaries", "SailorEngine-Release");
#endif
            }
        }

        public Rect Viewport
        {
            get
            {
                lock (sceneViewportStateLock)
                {
                    return sceneViewportState.Capture().Rect;
                }
            }
            set
            {
                lock (sceneViewportStateLock)
                {
                    sceneViewportState.RememberRect(value);
                }
            }
        }

        public event Action<string[]> OnPullMessagesAction = delegate { };
        public event Action<string> OnUpdateCurrentWorldAction = delegate { };

        public EngineTypes EngineTypes => Volatile.Read(ref editorTypes);

        public void ResetForWorkspaceChange()
        {
            if (State is EngineLifecycleState.Starting or EngineLifecycleState.Running or EngineLifecycleState.Stopping)
            {
                throw new InvalidOperationException("Workspace engine metadata can only be reset after the engine has stopped.");
            }

            Interlocked.Increment(ref engineGeneration);
            Volatile.Write(ref editorTypes, new EngineTypes());
            consoleMessages.DrainPending();
        }

        public string[] GetRecentConsoleMessages()
        {
            var generation = Volatile.Read(ref engineGeneration);
            return consoleMessages.Snapshot()
                .Where(message => message.Generation == generation)
                .Select(message => message.Text)
                .ToArray();
        }

        public void PushConsoleMessage(string message)
        {
            if (string.IsNullOrWhiteSpace(message))
            {
                return;
            }

            PublishConsoleMessages([message], Volatile.Read(ref engineGeneration));
        }

        void PublishConsoleMessages(string[] messages, long generation)
        {
            if (messages.Length == 0)
            {
                return;
            }

            var filtered = messages.Where(message => !string.IsNullOrWhiteSpace(message)).ToArray();
            if (filtered.Length == 0)
            {
                return;
            }

            consoleMessages.EnqueueRange(filtered.Select(message => new ConsoleMessage(generation, message)));
            ScheduleConsoleDispatch();
        }

        void ScheduleConsoleDispatch()
        {
            if (Interlocked.Exchange(ref consoleDispatchScheduled, 1) != 0)
            {
                return;
            }

            MainThread.BeginInvokeOnMainThread(FlushConsoleMessages);
        }

        void FlushConsoleMessages()
        {
            try
            {
                var generation = Volatile.Read(ref engineGeneration);
                var pending = consoleMessages.DrainPending()
                    .Where(message => message.Generation == generation)
                    .Select(message => message.Text)
                    .ToArray();
                if (pending.Length > 0)
                {
                    OnPullMessagesAction?.Invoke(pending);
                }
            }
            finally
            {
                Interlocked.Exchange(ref consoleDispatchScheduled, 0);
                if (consoleMessages.HasPending)
                {
                    ScheduleConsoleDispatch();
                }
            }
        }

        void SetLifecycleState(EngineLifecycleState state)
        {
            Volatile.Write(ref lifecycleState, (int)state);
            MainThread.BeginInvokeOnMainThread(() =>
            {
                if (State == state)
                {
                    OnLifecycleStateChanged?.Invoke(state);
                }
            });
        }

        bool IsGenerationActive(long generation, bool allowStarting = false)
        {
            if (Volatile.Read(ref engineGeneration) != generation)
            {
                return false;
            }

            var state = State;
            return state == EngineLifecycleState.Running ||
                (allowStarting && state == EngineLifecycleState.Starting);
        }

        bool IsInteropRunning() => State == EngineLifecycleState.Running;

        async Task<EngineProtocolAssetReloadState?> TryGetAssetReloadStateAsync(
            long generation,
            CancellationToken cancellationToken = default)
        {
            if (!IsGenerationActive(generation))
            {
                return null;
            }

            var state = await protocolClient.GetAssetReloadStateAsync(
                cancellationToken).ConfigureAwait(false);
            return IsGenerationActive(generation) && state.Available
                ? state
                : null;
        }

        void PublishAssetReloadCompletion(AssetReloadCompletion completion, long generation)
        {
            MainThread.BeginInvokeOnMainThread(() =>
            {
                if (IsGenerationActive(generation))
                {
                    OnAssetReloadCompleted?.Invoke(completion);
                }
            });
        }

        void PublishEditorViewportEvents(
            IReadOnlyList<EditorViewportEvent> viewportEvents,
            long generation,
            long eventEpoch)
        {
            if (viewportEvents.Count == 0)
            {
                return;
            }

            MainThread.BeginInvokeOnMainThread(() =>
            {
                if (IsGenerationActive(generation) && editorViewportEventEpoch.IsCurrent(eventEpoch))
                {
                    OnEditorViewportEvents?.Invoke(viewportEvents);
                }
            });
        }

        async Task<bool> InvokeRunningInteropAsync(
            Func<CancellationToken, Task<bool>> action,
            bool invalidateQueuedWorldSnapshots = false,
            CancellationToken cancellationToken = default)
        {
            ArgumentNullException.ThrowIfNull(action);
            if (!IsInteropRunning() ||
                !await action(cancellationToken).ConfigureAwait(false))
            {
                return false;
            }

            if (invalidateQueuedWorldSnapshots)
            {
                var mutationSequence = worldSnapshotPublication.ReserveSequence();
                worldSnapshotPublication.TryAdvance(mutationSequence);
            }

            return true;
        }

        bool QueuePlatformInterop(Func<CancellationToken, ValueTask<bool>> action)
        {
            ArgumentNullException.ThrowIfNull(action);
            var generation = Volatile.Read(ref engineGeneration);
            if (!IsGenerationActive(generation))
            {
                return false;
            }

            lock (platformInteropQueueLock)
            {
                if (pendingPlatformInteropCommands >=
                    MaxPendingPlatformInteropCommands)
                {
                    return false;
                }
                pendingPlatformInteropCommands++;
                platformInteropQueue = ExecuteQueuedPlatformInteropAsync(
                    platformInteropQueue,
                    action,
                    generation);
            }
            return true;
        }

        bool QueuePlatformInterop(Func<ValueTask<bool>> action)
        {
            ArgumentNullException.ThrowIfNull(action);
            return QueuePlatformInterop(_ => action());
        }

        async Task ExecuteQueuedPlatformInteropAsync(
            Task predecessor,
            Func<CancellationToken, ValueTask<bool>> action,
            long generation)
        {
            await Task.Yield();
            try
            {
                await predecessor.ConfigureAwait(false);
                if (!IsGenerationActive(generation))
                {
                    return;
                }

                CancellationToken cancellationToken;
                lock (runLock)
                {
                    cancellationToken =
                        activeSession?.PollCancellation.Token ??
                        disposeCancellation.Token;
                }
                await action(cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
                when (!IsGenerationActive(generation) ||
                    disposeCancellation.IsCancellationRequested)
            {
            }
            catch (Exception ex)
            {
                Console.WriteLine(
                    $"[EngineService] Platform interop command failed: {ex.Message}");
            }
            finally
            {
                Interlocked.Decrement(ref pendingPlatformInteropCommands);
            }
        }

        Task CapturePlatformInteropQueue()
        {
            lock (platformInteropQueueLock)
            {
                return platformInteropQueue;
            }
        }

        public Task<bool> IsEditorViewportSelectionEventCurrentAsync(
            ulong managedMutationRevision)
            => IsEditorViewportEventCurrentAsync(
                EditorManagedMutationKind.Selection,
                string.Empty,
                managedMutationRevision);

        public Task<bool> IsEditorViewportTransformEventCurrentAsync(
            string instanceId,
            ulong managedMutationRevision)
            => IsEditorViewportEventCurrentAsync(
                EditorManagedMutationKind.ObjectTransform,
                instanceId,
                managedMutationRevision);

        async Task<bool> IsEditorViewportEventCurrentAsync(
            EditorManagedMutationKind kind,
            string instanceId,
            ulong managedMutationRevision)
        {
            if (!IsInteropRunning())
            {
                return false;
            }

            var currentRevision =
                await protocolClient.GetEditorManagedMutationRevisionAsync(
                    (uint)kind,
                    instanceId).ConfigureAwait(false);
            return IsInteropRunning() &&
                EditorViewportMutationOrder.IsCurrent(
                    managedMutationRevision,
                    currentRevision);
        }

        public void InvalidateQueuedWorldSnapshots()
        {
            var mutationSequence = worldSnapshotPublication.ReserveSequence();
            worldSnapshotPublication.TryAdvance(mutationSequence);
        }

        public static void ShowMainWindow(bool bShow)
        {
            var instance = Volatile.Read(ref currentInstance);
            if (instance is null || !instance.IsRunning)
            {
                return;
            }

#if WINDOWS || MACCATALYST
            instance.QueuePlatformInterop(async cancellationToken =>
            {
                await instance.protocolClient.ShowMainWindowAsync(
                    bShow,
                    cancellationToken).ConfigureAwait(false);
                return true;
            });
#else
            if (instance.IsInteropRunning())
            {
                _ = instance.protocolClient.ShowMainWindowAsync(bShow);
            }
#endif
        }

        static string ResolveRepoRoot()
        {
            var current = new DirectoryInfo(AppContext.BaseDirectory);
            while (current != null)
            {
                if (Directory.Exists(Path.Combine(current.FullName, "Content")) &&
                    Directory.Exists(Path.Combine(current.FullName, "Editor")) &&
                    Directory.Exists(Path.Combine(current.FullName, "Runtime")))
                {
                    return current.FullName;
                }

                current = current.Parent;
            }

            return Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", ".."));
        }

        public void BindMacRemoteViewportHost(
            ulong viewportId,
            nint hostHandle,
            double compositionScale = 1)
        {
#if WINDOWS
            if (!IsRunning)
            {
                return;
            }

            try
            {
                if (EngineProtocolNative.SailorProtocolSetWindowsViewportHost(
                        viewportId,
                        hostHandle,
                        double.IsFinite(compositionScale) && compositionScale > 0
                            ? (float)compositionScale
                            : 1.0f) != 0)
                {
                    Interlocked.Exchange(ref windowsViewportInteropFailureLogged, 0);
                }
            }
            catch (Exception exception)
            {
                if (Interlocked.Exchange(ref windowsViewportInteropFailureLogged, 1) == 0)
                {
                    Console.WriteLine(
                        $"[EngineService] Windows viewport host binding failed: {exception}");
                }
            }
#elif MACCATALYST
            var generation = Volatile.Read(ref engineGeneration);
            if (!IsGenerationActive(generation))
            {
                lock (macRemoteViewportHostLock)
                {
                    appliedMacRemoteViewportHosts.Remove(viewportId);
                }
                return;
            }

            lock (macRemoteViewportHostLock)
            {
                if (appliedMacRemoteViewportHosts.TryGetValue(viewportId, out var applied) &&
                    applied.Generation == generation &&
                    applied.Handle == hostHandle)
                {
                    return;
                }

                appliedMacRemoteViewportHosts[viewportId] =
                    (generation, hostHandle);
            }

            if (!QueuePlatformInterop(async cancellationToken =>
            {
                if (await protocolClient.SetRemoteViewportMacHostHandleAsync(
                        viewportId,
                        2u,
                        (ulong)hostHandle,
                        cancellationToken).ConfigureAwait(false))
                {
                    return true;
                }

                lock (macRemoteViewportHostLock)
                {
                    if (appliedMacRemoteViewportHosts.TryGetValue(
                            viewportId,
                            out var pending) &&
                        pending.Generation == generation &&
                        pending.Handle == hostHandle)
                    {
                        appliedMacRemoteViewportHosts.Remove(viewportId);
                    }
                }
                return false;
            }))
            {
                lock (macRemoteViewportHostLock)
                {
                    appliedMacRemoteViewportHosts.Remove(viewportId);
                }
            }
#endif
        }

        public bool TryUpdateRemoteViewport(ulong viewportId, Rect rect, bool visible, bool focused)
        {
#if WINDOWS || MACCATALYST
            lock (sceneViewportStateLock)
            {
                if (viewportId == SceneViewportId)
                {
                    sceneViewportState.Remember(rect, visible, focused);
                }
            }

            return !rect.IsEmpty &&
                remoteViewportUpdates.Enqueue(
                    viewportId,
                    new RemoteViewportUpdate(
                        viewportId,
                        rect,
                        visible,
                        focused),
                    QueuePlatformInterop,
                    async update =>
                        await protocolClient.UpsertRemoteViewportAsync(
                            update.ViewportId,
                            (uint)update.Rect.X,
                            (uint)update.Rect.Y,
                            (uint)update.Rect.Width,
                            (uint)update.Rect.Height,
                            update.Visible,
                            update.Focused).ConfigureAwait(false));
#else
            return false;
#endif
        }

        async Task<bool> TryRefreshSceneRemoteViewportAsync(
            long generation,
            CancellationToken cancellationToken = default)
        {
#if WINDOWS
            SceneViewportStateSnapshot<Rect> viewportState;
            lock (sceneViewportStateLock)
            {
                viewportState = sceneViewportState.Capture();
            }
            if (!IsGenerationActive(generation))
            {
                return false;
            }
            if (await TryUpdateRemoteViewportAsync(
                    SceneViewportId,
                    viewportState.Rect,
                    viewportState.Visible,
                    viewportState.Focused,
                    cancellationToken).ConfigureAwait(false))
            {
                return true;
            }

            if (!viewportState.Rect.IsEmpty)
            {
                await protocolClient.SetViewportAsync(
                        (uint)viewportState.Rect.X,
                        (uint)viewportState.Rect.Y,
                        (uint)viewportState.Rect.Width,
                        (uint)viewportState.Rect.Height,
                        cancellationToken).ConfigureAwait(false);
            }

            return false;
#else
            await Task.CompletedTask;
            return false;
#endif
        }

        async Task<bool> TryUpdateRemoteViewportAsync(
            ulong viewportId,
            Rect rect,
            bool visible,
            bool focused,
            CancellationToken cancellationToken = default)
        {
#if WINDOWS || MACCATALYST
            return IsInteropRunning() &&
                !rect.IsEmpty &&
                await protocolClient.UpsertRemoteViewportAsync(
                    viewportId,
                    (uint)rect.X,
                    (uint)rect.Y,
                    (uint)rect.Width,
                    (uint)rect.Height,
                    visible,
                    focused,
                    cancellationToken).ConfigureAwait(false);
#else
            await Task.CompletedTask;
            return false;
#endif
        }

        public void SetViewport(Rect rect)
        {
            if (rect.IsEmpty)
            {
                return;
            }

#if WINDOWS || MACCATALYST
            lock (sceneViewportStateLock)
            {
                sceneViewportState.RememberRect(rect);
            }
            editorViewportUpdate.Enqueue(
                rect,
                QueuePlatformInterop,
                async update =>
                {
                    await protocolClient.SetViewportAsync(
                        (uint)update.X,
                        (uint)update.Y,
                        (uint)update.Width,
                        (uint)update.Height).ConfigureAwait(false);
                    return true;
                });
#else
            lock (sceneViewportStateLock)
            {
                sceneViewportState.RememberRect(rect);
            }
            if (IsInteropRunning())
            {
                QueuePlatformInterop(async cancellationToken =>
                {
                    await protocolClient.SetViewportAsync(
                        (uint)rect.X,
                        (uint)rect.Y,
                        (uint)rect.Width,
                        (uint)rect.Height,
                        cancellationToken).ConfigureAwait(false);
                    return true;
                });
            }
#endif
        }

        public void SetEditorRenderTargetSize(uint width, uint height)
        {
#if MACCATALYST
            renderTargetUpdate.Enqueue(
                (Math.Max(width, 1u), Math.Max(height, 1u)),
                QueuePlatformInterop,
                async size =>
                {
                    await protocolClient.SetEditorRenderTargetSizeAsync(
                        size.Width,
                        size.Height).ConfigureAwait(false);
                    return true;
                });
#endif
        }

        public void DestroyRemoteViewport(ulong viewportId)
        {
#if WINDOWS || MACCATALYST
            viewportStatusRefreshes.Reset(viewportId);
            lock (remoteViewportStateLock)
            {
                remoteViewportStates[viewportId] =
                    RemoteViewportSessionState.Terminating;
                remoteViewportDiagnostics.Remove(viewportId);
            }
            QueuePlatformInterop(async cancellationToken =>
            {
                var destroyed =
                    await protocolClient.DestroyRemoteViewportAsync(
                        viewportId,
                        cancellationToken).ConfigureAwait(false);
                lock (remoteViewportStateLock)
                {
                    remoteViewportStates[viewportId] = destroyed
                        ? RemoteViewportSessionState.Disposed
                        : RemoteViewportSessionState.Lost;
                }
                return destroyed;
            });
#endif
        }

        public RemoteViewportSessionState GetRemoteViewportState(ulong viewportId)
        {
#if WINDOWS || MACCATALYST
            if (State != EngineLifecycleState.Running)
            {
                return RemoteViewportSessionState.Disposed;
            }
            QueueRemoteViewportStatusRefresh(viewportId);
            lock (remoteViewportStateLock)
            {
                return remoteViewportStates.TryGetValue(
                    viewportId,
                    out var state)
                    ? state
                    : RemoteViewportSessionState.Created;
            }
#else
            return RemoteViewportSessionState.Created;
#endif
        }

        public void RetryRemoteViewport(ulong viewportId)
        {
#if WINDOWS || MACCATALYST
            QueuePlatformInterop(cancellationToken =>
                new ValueTask<bool>(
                    protocolClient.RetryRemoteViewportAsync(
                        viewportId,
                        cancellationToken)));
#endif
        }

        public bool SendRemoteViewportInput(
            ulong viewportId,
            RemoteViewportInputKind kind,
            float pointerX = 0,
            float pointerY = 0,
            float wheelDeltaX = 0,
            float wheelDeltaY = 0,
            uint keyCode = 0,
            uint button = 0,
            RemoteViewportInputModifier modifiers = RemoteViewportInputModifier.None,
            bool pressed = false,
            bool focused = false,
            bool captured = false)
        {
#if WINDOWS || MACCATALYST
            var input = new RemoteViewportInput(
                viewportId,
                kind,
                pointerX,
                pointerY,
                wheelDeltaX,
                wheelDeltaY,
                keyCode,
                button,
                modifiers,
                pressed,
                focused,
                captured);
            async ValueTask<bool> SendInput(RemoteViewportInput value)
                => await protocolClient.SendRemoteViewportInputAsync(
                    value.ViewportId,
                    (uint)value.Kind,
                    value.PointerX,
                    value.PointerY,
                    value.WheelDeltaX,
                    value.WheelDeltaY,
                    value.KeyCode,
                    value.Button,
                    (uint)value.Modifiers,
                    value.Pressed,
                    value.Focused,
                    value.Captured).ConfigureAwait(false);

            return kind == RemoteViewportInputKind.PointerMove
                ? pointerMoves.Enqueue(
                    viewportId,
                    input,
                    QueuePlatformInterop,
                    SendInput)
                : QueuePlatformInterop(_ => SendInput(input));
#else
            return false;
#endif
        }

        public string GetRemoteViewportDiagnostics(ulong viewportId)
        {
#if WINDOWS || MACCATALYST
            if (State != EngineLifecycleState.Running)
            {
                return string.Empty;
            }
            QueueRemoteViewportStatusRefresh(viewportId);
            lock (remoteViewportStateLock)
            {
                return remoteViewportDiagnostics.TryGetValue(
                    viewportId,
                    out var diagnostics)
                    ? diagnostics
                    : string.Empty;
            }
#else
            return string.Empty;
#endif
        }

#if WINDOWS || MACCATALYST
        void QueueRemoteViewportStatusRefresh(ulong viewportId)
        {
            viewportStatusRefreshes.Enqueue(
                viewportId,
                viewportId,
                QueuePlatformInterop,
                async id =>
                {
                    var state = (RemoteViewportSessionState)
                        await protocolClient.GetRemoteViewportStateAsync(
                            id).ConfigureAwait(false);
                    var diagnostics =
                        await protocolClient.GetRemoteViewportDiagnosticsAsync(
                            id).ConfigureAwait(false);
                    lock (remoteViewportStateLock)
                    {
                        remoteViewportStates[id] = state;
                        remoteViewportDiagnostics[id] = diagnostics;
                    }
                    return true;
                });
        }

        void ResetPlatformInteropState()
        {
            remoteViewportUpdates.Reset();
            editorViewportUpdate.Reset();
            renderTargetUpdate.Reset();
            pointerMoves.Reset();
            viewportStatusRefreshes.Reset();
            lock (remoteViewportStateLock)
            {
                remoteViewportStates.Clear();
                remoteViewportDiagnostics.Clear();
            }
#if MACCATALYST
            lock (macRemoteViewportHostLock)
            {
                appliedMacRemoteViewportHosts.Clear();
            }
#endif
        }
#endif

        public Task StartAsync(EngineLaunchContext launchContext, CancellationToken cancellationToken = default)
            => StartAsync(launchContext, false, Array.Empty<string>(), cancellationToken);

        public async Task StartAsync(
            EngineLaunchContext launchContext,
            bool bDebug,
            IEnumerable<string>? commandLineArgs,
            CancellationToken cancellationToken = default)
        {
            ArgumentNullException.ThrowIfNull(launchContext);

#if !WINDOWS && !MACCATALYST
            throw new PlatformNotSupportedException("The in-process engine is supported only by the Windows and Mac Catalyst editor hosts.");
#else
            ObjectDisposedException.ThrowIf(
                Volatile.Read(ref disposeState) != 0,
                this);
            using var startCancellation =
                CancellationTokenSource.CreateLinkedTokenSource(
                    cancellationToken,
                    disposeCancellation.Token);
            var startCancellationToken = startCancellation.Token;
            while (true)
            {
                await lifecycleGate.WaitAsync(startCancellationToken).ConfigureAwait(false);
                Task? stoppingTask = null;
                try
                {
                    if (State == EngineLifecycleState.Stopping)
                    {
                        lock (runLock)
                        {
                            stoppingTask = activeSession?.CompletionTask ?? Task.CompletedTask;
                        }
                    }
                    else
                    {
                        break;
                    }
                }
                finally
                {
                    if (stoppingTask is not null)
                    {
                        lifecycleGate.Release();
                    }
                }

                if (stoppingTask is not null)
                {
                    await stoppingTask.WaitAsync(startCancellationToken).ConfigureAwait(false);
                }
            }

            var initialized = false;
            var startRequested = false;
            var generation = 0L;
            Task? runtimeMonitorTask = null;
            CancellationTokenSource? runtimeMonitorCancellation = null;
            CancellationTokenRegistration startupMonitorCancellation = default;
            var runtimeMonitorCancellationOwnedBySession = false;
            if (State == EngineLifecycleState.Running)
            {
                try
                {
                    if (Equals(ActiveLaunchContext, launchContext))
                    {
                        return;
                    }

                    throw new InvalidOperationException("The engine is already running with a different launch context. Stop it before starting another workspace.");
                }
                finally
                {
                    lifecycleGate.Release();
                }
            }

            try
            {
                startCancellationToken.ThrowIfCancellationRequested();
                generation = Interlocked.Increment(ref engineGeneration);
#if WINDOWS || MACCATALYST
                ResetPlatformInteropState();
#endif
                Volatile.Write(ref lastExitCode, 0);
                lock (runLock)
                {
                    activeLaunchContext = launchContext;
                    lastFailure = null;
                }
                SetLifecycleState(EngineLifecycleState.Starting);

                var args = await BuildInteropArgumentsAsync(launchContext, bDebug, commandLineArgs).ConfigureAwait(false);
                Console.WriteLine($"Starting SailorEngine interop with workspace: {launchContext.WorkspaceRoot}");
                Volatile.Write(ref editorTypes, new EngineTypes());

#if WINDOWS || MACCATALYST
                await MainThread.InvokeOnMainThreadAsync(
                    () => protocolClient.InitializeAsync(
                        args,
                        startCancellationToken));
#else
                await protocolClient.InitializeAsync(
                    args,
                    startCancellationToken).ConfigureAwait(false);
#endif
                initialized = true;
                runtimeMonitorCancellation =
                    CancellationTokenSource.CreateLinkedTokenSource(
                        disposeCancellation.Token);
                startupMonitorCancellation = startCancellationToken.Register(
                    static state =>
                        ((CancellationTokenSource)state!).Cancel(),
                    runtimeMonitorCancellation);
                var monitorCancellation = runtimeMonitorCancellation;
                startRequested = true;
                await protocolClient.StartAsync(
                    monitorCancellation.Token).ConfigureAwait(false);
                await WaitForEngineMainThreadAsync(
                    startCancellationToken).ConfigureAwait(false);
                runtimeMonitorTask = MonitorEngineLifetimeAsync(
                    generation,
                    launchContext,
                    monitorCancellation.Token);

                var initializationExitCode =
                    await ReadNativeExitCodeAsync(
                        startCancellationToken).ConfigureAwait(false);
                if (initializationExitCode != 0)
                {
                    throw new EngineLifecycleException(
                        $"SailorEngine initialization failed for workspace '{launchContext.WorkspaceRoot}' with exit code {initializationExitCode}.",
                        initializationExitCode);
                }

                var editorTypeCacheIdentity =
                    await ReadWorkspaceCacheIdentityAsync(
                    generation,
                    launchContext,
                    allowStarting: true,
                    cancellationToken: startCancellationToken)
                    .ConfigureAwait(false);
                var cachedEditorTypes = editorTypeCacheStore.Load(
                    launchContext.EditorTypesCacheFilePath,
                    editorTypeCacheIdentity);
                if (cachedEditorTypes.Succeeded)
                {
                    if (!TryParseEditorTypes(
                            cachedEditorTypes.Payload!,
                            out _,
                            out var cachedCatalogError))
                    {
                        Console.WriteLine(
                            $"Editor type cache payload is invalid: {cachedCatalogError}");
                        LogEditorTypeCacheInvalidation(
                            launchContext.EditorTypesCacheFilePath,
                            "invalid cached catalog");
                    }
                }
                else if (EditorTypeCacheStore.ShouldInvalidate(cachedEditorTypes.Status))
                {
                    Console.WriteLine(cachedEditorTypes.Diagnostic);
                    LogEditorTypeCacheInvalidation(
                        launchContext.EditorTypesCacheFilePath,
                        cachedEditorTypes.Status.ToString());
                }
                else if (cachedEditorTypes.Status == EditorTypeCacheStatus.IoFailure)
                {
                    Console.WriteLine(cachedEditorTypes.Diagnostic);
                }

                // Required startup order: combined editor catalog, world, then initial messages.
                string serializedEditorTypes =
                    await SerializeEditorTypesAsync(
                    generation,
                    allowStarting: true,
                    cancellationToken: startCancellationToken)
                    .ConfigureAwait(false);
                if (TryParseEditorTypes(
                        serializedEditorTypes,
                        out var liveEditorTypes,
                        out var catalogError))
                {
                    Volatile.Write(ref editorTypes, liveEditorTypes);
                    if (EditorTypeCacheStore.ShouldPersistLiveCatalog(cachedEditorTypes.Status))
                    {
                        var cacheWrite = editorTypeCacheStore.Save(
                            launchContext.EditorTypesCacheFilePath,
                            editorTypeCacheIdentity,
                            serializedEditorTypes);
                        if (!cacheWrite.Succeeded)
                            Console.WriteLine(cacheWrite.Diagnostic);
                    }
                    else
                    {
                        Console.WriteLine(
                            $"Preserving editor type cache after {cachedEditorTypes.Status}: '{launchContext.EditorTypesCacheFilePath}'.");
                    }
                }
                else
                {
                    Volatile.Write(ref editorTypes, new EngineTypes());
                    if (cachedEditorTypes.Status != EditorTypeCacheStatus.IoFailure)
                    {
                        LogEditorTypeCacheInvalidation(
                            launchContext.EditorTypesCacheFilePath,
                            "invalid live catalog");
                    }
                    throw new EngineLifecycleException(
                        $"SailorEngine returned an invalid editor type catalog: {catalogError}",
                        -1);
                }

                string serializedWorld;
                long serializedWorldSequence = 0;
#if MACCATALYST
                var serializedWorldResult =
                    await MainThread.InvokeOnMainThreadAsync(
                    () => SerializeWorldAsync(
                        generation,
                        allowStarting: true,
                        cancellationToken: startCancellationToken));
                serializedWorld = serializedWorldResult.SerializedWorld;
                serializedWorldSequence = serializedWorldResult.Sequence;
#else
                var serializedWorldResult =
                    await SerializeWorldAsync(
                    generation,
                    allowStarting: true,
                    cancellationToken: startCancellationToken)
                    .ConfigureAwait(false);
                serializedWorld = serializedWorldResult.SerializedWorld;
                serializedWorldSequence = serializedWorldResult.Sequence;
#endif
                QueueWorldUpdate(serializedWorld, generation, serializedWorldSequence);

                var bootstrapMessages = await PullMessagesAsync(
                    generation,
                    allowStarting: true,
                    cancellationToken: startCancellationToken)
                    .ConfigureAwait(false);
                if (bootstrapMessages is not null)
                {
                    PublishConsoleMessages(bootstrapMessages, generation);
                }

                var pollCancellation = new CancellationTokenSource();
                var backgroundCancellation = new CancellationTokenSource();
                var pollTasks = new List<Task>();
#if !MACCATALYST
                pollTasks.Add(RunPeriodicTaskAsync(async () =>
                {
                    await TryRefreshSceneRemoteViewportAsync(
                        generation,
                        pollCancellation.Token).ConfigureAwait(false);
                }, 500, 100, pollCancellation.Token, generation));
#endif
                pollTasks.Add(RunPeriodicTaskAsync(async () =>
                {
                    var messages = await PullMessagesAsync(
                        generation,
                        cancellationToken: pollCancellation.Token)
                        .ConfigureAwait(false);
                    if (messages is not null)
                    {
                        PublishConsoleMessages(messages, generation);
                    }
                }, 300, 500, pollCancellation.Token, generation));

                pollTasks.Add(RunPeriodicTaskAsync(async () =>
                {
                    var (viewportEvents, eventEpoch) =
                        await PullEditorViewportEventsAsync(
                        generation,
                        pollCancellation.Token).ConfigureAwait(false);
                    if (viewportEvents.Count > 0)
                    {
                        PublishEditorViewportEvents(viewportEvents, generation, eventEpoch);
                    }
                }, 33, 33, pollCancellation.Token, generation));

                ulong lastAssetReloadCompletion = 0;
                pollTasks.Add(RunPeriodicTaskAsync(async () =>
                {
                    var reloadState =
                        await TryGetAssetReloadStateAsync(
                            generation,
                            pollCancellation.Token).ConfigureAwait(false);
                    if (reloadState is { } state &&
                        state.CompletedGeneration > lastAssetReloadCompletion &&
                        state.CompletedGeneration <= state.RequestGeneration)
                    {
                        lastAssetReloadCompletion = state.CompletedGeneration;
                        PublishAssetReloadCompletion(
                            new AssetReloadCompletion(
                                state.CompletedGeneration,
                                state.SuccessfulGeneration ==
                                    state.CompletedGeneration),
                            generation);
                    }
                }, 100, 100, pollCancellation.Token, generation));

                pollTasks.Add(RunPeriodicTaskAsync(async () =>
                {
                    var serializedWorldResult =
                        await SerializeWorldAsync(
                        generation,
                        cancellationToken: pollCancellation.Token)
                        .ConfigureAwait(false);
                    QueueWorldUpdate(
                        serializedWorldResult.SerializedWorld,
                        generation,
                        serializedWorldResult.Sequence);
                }, 1500, 0, pollCancellation.Token, generation));

                var session = new EngineSession
                {
                    Generation = generation,
                    LaunchContext = launchContext,
                    PollCancellation = pollCancellation,
                    BackgroundCancellation = backgroundCancellation,
                    RuntimeMonitorCancellation = runtimeMonitorCancellation,
                    RuntimeMonitorTask = runtimeMonitorTask,
                    PollTasks = pollTasks
                };

                lock (runLock)
                {
                    activeSession = session;
                }
                SetLifecycleState(EngineLifecycleState.Running);
                session.CompletionTask = CompleteSessionAsync(session);
                runtimeMonitorCancellationOwnedBySession = true;
            }
            catch (Exception startupException)
            {
                var failure = startupException;
                var exitCode = 0;
                if (initialized)
                {
                    try
                    {
                        exitCode = await ReadNativeExitCodeAsync()
                            .ConfigureAwait(false);
                    }
                    catch (Exception exitCodeException)
                    {
                        failure = new AggregateException(
                            "Engine startup failed and its native exit code could not be read.",
                            failure,
                            exitCodeException);
                    }

                    try
                    {
                        await ShutdownNativeAfterFailedStartAsync(
                            startRequested,
                            runtimeMonitorTask,
                            runtimeMonitorCancellation).ConfigureAwait(false);
                    }
                    catch (Exception teardownException)
                    {
                        failure = new AggregateException(
                            "Engine startup failed and native teardown was incomplete.",
                            failure,
                            teardownException);
                    }
                }

                if (exitCode == 0 && startupException is EngineLifecycleException lifecycleException)
                {
                    exitCode = lifecycleException.ExitCode;
                }
                var cleanCancellation =
                    startupException is OperationCanceledException &&
                    startCancellationToken.IsCancellationRequested &&
                    ReferenceEquals(failure, startupException);
                if (exitCode == 0 && !cleanCancellation)
                {
                    exitCode = -1;
                }

                Volatile.Write(ref lastExitCode, exitCode);
                Interlocked.Increment(ref engineGeneration);
                lock (runLock)
                {
                    activeLaunchContext = null;
                    activeSession = null;
                    lastFailure = cleanCancellation ? null : failure;
                }
                SetLifecycleState(cleanCancellation
                    ? EngineLifecycleState.Stopped
                    : EngineLifecycleState.Faulted);
                Volatile.Write(ref editorTypes, new EngineTypes());

                if (cleanCancellation)
                {
                    throw;
                }
                if (ReferenceEquals(failure, startupException) && startupException is EngineLifecycleException)
                {
                    throw;
                }

                throw new EngineLifecycleException(
                    $"SailorEngine startup failed for workspace '{launchContext.WorkspaceRoot}'.",
                    exitCode,
                    failure);
            }
            finally
            {
                startupMonitorCancellation.Dispose();
                if (!runtimeMonitorCancellationOwnedBySession)
                {
                    runtimeMonitorCancellation?.Cancel();
                    runtimeMonitorCancellation?.Dispose();
                }
                lifecycleGate.Release();
            }
#endif
        }

        public async Task<int> StopAsync(CancellationToken cancellationToken = default)
        {
#if !WINDOWS && !MACCATALYST
            return LastExitCode;
#else
            await lifecycleGate.WaitAsync(cancellationToken).ConfigureAwait(false);
            Task? completionTask;
            EngineSession? session;
            Exception? stopFailure = null;
            try
            {
                lock (runLock)
                {
                    session = activeSession;
                    completionTask = session?.CompletionTask;
                }

                if (completionTask is null)
                {
                    // A prior failed session has already completed native teardown.
                    // Its historical failure must not prevent a later candidate
                    // workspace from entering the repair pipeline and restarting.
                    return LastExitCode;
                }

                if (State != EngineLifecycleState.Stopping)
                {
                    SetLifecycleState(EngineLifecycleState.Stopping);
                    Interlocked.Increment(ref engineGeneration);
                    session!.PollCancellation.Cancel();
                    session.BackgroundCancellation.Cancel();

                    // The Start request was already acknowledged. Cancel the
                    // liveness monitor before stopping so an expected readiness
                    // transition cannot be reported as a runtime failure.
                    session.RuntimeMonitorCancellation.Cancel();
                    stopFailure = await RequestNativeStopAsync()
                        .ConfigureAwait(false);
                }
            }
            finally
            {
                lifecycleGate.Release();
            }

            // Teardown is deliberately non-cancellable once native shutdown starts.
            try
            {
                await completionTask.WaitAsync(
                    TimeSpan.FromSeconds(30)).ConfigureAwait(false);
            }
            catch (TimeoutException timeoutException)
            {
                Exception? fallbackFailure = null;
                try
                {
                    await protocolClient.RequestLocalStopFallbackAsync()
                        .ConfigureAwait(false);
                }
                catch (Exception ex)
                {
                    fallbackFailure = ex;
                }
                try
                {
                    await completionTask.WaitAsync(
                        TimeSpan.FromSeconds(5)).ConfigureAwait(false);
                }
                catch (Exception ex)
                {
                    fallbackFailure = CombineFailures(
                        fallbackFailure,
                        ex);
                }
                stopFailure = CombineFailures(
                    stopFailure,
                    timeoutException);
                if (fallbackFailure is not null)
                {
                    stopFailure = CombineFailures(
                        stopFailure,
                        fallbackFailure);
                }
            }
            if (stopFailure is not null)
            {
                lock (runLock)
                {
                    lastFailure ??= stopFailure;
                }
                SetLifecycleState(EngineLifecycleState.Faulted);
            }
            return EnsureSuccessfulStop(stopFailure);
#endif
        }

        public async Task RestartAsync(
            EngineLaunchContext launchContext,
            string serializedWorld,
            SceneViewportToolState? viewportToolState = null,
            CancellationToken cancellationToken = default)
        {
            ArgumentNullException.ThrowIfNull(launchContext);
            if (string.IsNullOrWhiteSpace(serializedWorld))
            {
                throw new ArgumentException(
                    "A serialized recovery world is required to restart the Engine.",
                    nameof(serializedWorld));
            }

#if !WINDOWS && !MACCATALYST
            throw new PlatformNotSupportedException(
                "The local Engine host is supported only by the Windows and Mac Catalyst editor hosts.");
#else
            await restartGate.WaitAsync(cancellationToken).ConfigureAwait(false);
            try
            {
                cancellationToken.ThrowIfCancellationRequested();
                Directory.CreateDirectory(launchContext.CacheDirectory);
                await File.WriteAllTextAsync(
                        launchContext.TempWorldFilePath,
                        serializedWorld,
                        cancellationToken)
                    .ConfigureAwait(false);

                // Once teardown starts, complete the restart transaction even if
                // the UI request is cancelled. Leaving the local host half-stopped
                // would make the next explicit recovery attempt unreliable.
                if (State is EngineLifecycleState.Starting or
                    EngineLifecycleState.Running or
                    EngineLifecycleState.Stopping)
                {
                    await StopAsync(CancellationToken.None).ConfigureAwait(false);
                }

                await StartAsync(
                        launchContext,
                        false,
                        ["--world", launchContext.TempWorldRuntimePath],
                        CancellationToken.None)
                    .ConfigureAwait(false);

                if (viewportToolState is { } toolState &&
                    !await SetViewportToolStateAsync(
                            toolState.Operation,
                            toolState.Space,
                            CancellationToken.None)
                        .ConfigureAwait(false))
                {
                    Console.WriteLine(
                        "[EngineService] Engine restarted, but the Scene viewport tool state could not be restored.");
                }
            }
            finally
            {
                restartGate.Release();
            }
#endif
        }

        int EnsureSuccessfulStop(Exception? stopFailure = null)
        {
            var exitCode = LastExitCode;
            var failure = stopFailure ?? LastFailure;
            if (exitCode != 0 || failure is not null || State == EngineLifecycleState.Faulted)
            {
                if (exitCode == 0)
                {
                    exitCode = -1;
                }
                throw new EngineLifecycleException(
                    $"SailorEngine shutdown failed with exit code {exitCode}.",
                    exitCode,
                    failure);
            }
            return exitCode;
        }

        async Task CompleteSessionAsync(EngineSession session)
        {
            Exception? failure = null;
            try
            {
                await session.RuntimeMonitorTask.ConfigureAwait(false);
            }
            catch (OperationCanceledException)
                when (session.RuntimeMonitorCancellation.IsCancellationRequested)
            {
            }
            catch (Exception ex)
            {
                failure = ex;
            }

            await lifecycleGate.WaitAsync().ConfigureAwait(false);
            try
            {
                lock (runLock)
                {
                    if (!ReferenceEquals(activeSession, session))
                    {
                        return;
                    }
                }

                session.PollCancellation.Cancel();
                session.BackgroundCancellation.Cancel();
                if (State == EngineLifecycleState.Running)
                {
                    using var exitCodeCancellation =
                        new CancellationTokenSource(
                            LifecycleProbeTimeout);
                    var exitCode =
                        await ReadNativeExitCodeSafelyAsync(
                            exitCodeCancellation.Token).ConfigureAwait(false);
                    failure ??= new EngineLifecycleException(
                        $"SailorEngine exited unexpectedly for workspace '{session.LaunchContext.WorkspaceRoot}'.",
                        exitCode);
                    SetLifecycleState(EngineLifecycleState.Stopping);
                    Interlocked.Increment(ref engineGeneration);
                }
            }
            finally
            {
                lifecycleGate.Release();
            }

            try
            {
                await Task.WhenAll(session.PollTasks).ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                failure ??= ex;
            }

            try
            {
                await CapturePlatformInteropQueue().ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                failure ??= ex;
            }

            await lifecycleGate.WaitAsync().ConfigureAwait(false);
            try
            {
                lock (runLock)
                {
                    if (!ReferenceEquals(activeSession, session))
                    {
                        return;
                    }
                }

                var exitCode = 0;
                try
                {
                    using var exitCodeCancellation =
                        new CancellationTokenSource(
                            LifecycleProbeTimeout);
                    exitCode = await ReadNativeExitCodeAsync(
                        exitCodeCancellation.Token).ConfigureAwait(false);
                }
                catch (Exception ex)
                {
                    failure = CombineFailures(failure, ex);
                }

                var shutdownFailure = await ShutdownNativeSessionAsync(
                    stopNative: false,
                    destroyRemoteViewport: true).ConfigureAwait(false);
                if (shutdownFailure is not null)
                {
                    failure = CombineFailures(failure, shutdownFailure);
                }

                if (exitCode == 0 && failure is not null)
                {
                    exitCode = -1;
                }
                Volatile.Write(ref lastExitCode, exitCode);
                lock (runLock)
                {
                    activeSession = null;
                    activeLaunchContext = null;
                    lastFailure = failure;
                }
                SetLifecycleState(failure is null && exitCode == 0
                    ? EngineLifecycleState.Stopped
                    : EngineLifecycleState.Faulted);
            }
            finally
            {
                lifecycleGate.Release();
                session.PollCancellation.Dispose();
                session.BackgroundCancellation.Dispose();
                session.RuntimeMonitorCancellation.Dispose();
            }
        }

        async Task RunPeriodicTaskAsync(
            Func<Task> action,
            int initialDelay,
            int periodMs,
            CancellationToken token,
            long generation)
        {
            try
            {
                await Task.Delay(initialDelay, token).ConfigureAwait(false);
                while (!token.IsCancellationRequested && IsGenerationActive(generation))
                {
                    await action().ConfigureAwait(false);
                    if (periodMs == 0)
                    {
                        break;
                    }
                    await Task.Delay(periodMs, token).ConfigureAwait(false);
                }
            }
            catch (OperationCanceledException) when (token.IsCancellationRequested)
            {
            }
        }

        async Task<string[]> BuildInteropArgumentsAsync(
            EngineLaunchContext launchContext,
            bool bDebug,
            IEnumerable<string>? commandLineArgs)
        {
#if WINDOWS
            return await MainThread.InvokeOnMainThreadAsync(() =>
            {
                nint handle = 0;
                var window = Application.Current?.Windows?.FirstOrDefault();
                if (window?.Handler?.PlatformView is MauiWinUIWindow mauiWindow)
                {
                    handle = mauiWindow.WindowHandle;
                }
                if (handle == 0)
                {
                    throw new InvalidOperationException("Cannot resolve the engine host window handle.");
                }

                var extraArguments = new[]
                {
                    "--noconsole",
                    "--hwnd",
                    handle.ToInt64().ToString(CultureInfo.InvariantCulture),
                    "--editor"
                }.Concat(commandLineArgs ?? Array.Empty<string>());
                return launchContext.BuildInteropArguments(
                    bDebug ? PathToEngineExecDebug : PathToEngineExec,
                    launchContext.StartupWorld,
                    extraArguments).ToArray();
            });
#else
            var extraArguments = new[] { "--noconsole", "--editor" }
                .Concat(commandLineArgs ?? Array.Empty<string>());
            return launchContext.BuildInteropArguments(
                "SailorEditor",
                launchContext.StartupWorld,
                extraArguments).ToArray();
#endif
        }

        Task<int> ReadNativeExitCodeAsync(
            CancellationToken cancellationToken = default)
            => protocolClient.GetExitCodeAsync(cancellationToken);

        async Task<int> ReadNativeExitCodeSafelyAsync(
            CancellationToken cancellationToken = default)
        {
            try
            {
                return await ReadNativeExitCodeAsync(
                    cancellationToken).ConfigureAwait(false);
            }
            catch
            {
                return -1;
            }
        }

        static Exception CombineFailures(Exception? current, Exception next)
            => current is null ? next : new AggregateException(current, next);

        async Task WaitForEngineMainThreadAsync(
            CancellationToken cancellationToken)
        {
            var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(15);
            while (true)
            {
                cancellationToken.ThrowIfCancellationRequested();
                if (!await protocolClient.IsEngineRunningAsync(
                        cancellationToken).ConfigureAwait(false))
                {
                    throw new EngineLifecycleException(
                        "SailorEngine exited before its main thread became ready.",
                        await ReadNativeExitCodeSafelyAsync(
                            cancellationToken).ConfigureAwait(false));
                }
                if (await protocolClient.IsEngineMainThreadReadyAsync(
                        cancellationToken).ConfigureAwait(false))
                {
                    return;
                }

                if (DateTime.UtcNow >= deadline)
                {
                    throw new TimeoutException(
                        "Timed out waiting for the SailorEngine main thread.");
                }
                await Task.Delay(10, cancellationToken).ConfigureAwait(false);
            }
        }

        async Task MonitorEngineLifetimeAsync(
            long generation,
            EngineLaunchContext launchContext,
            CancellationToken cancellationToken)
        {
            while (true)
            {
                await Task.Delay(
                    RuntimeLivenessPollInterval,
                    cancellationToken).ConfigureAwait(false);
                if (!IsGenerationActive(generation, allowStarting: true))
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    return;
                }

                var isRunning =
                    await protocolClient.IsEngineRunningAsync(
                        cancellationToken).ConfigureAwait(false);
                if (!isRunning)
                {
                    throw new EngineLifecycleException(
                        $"SailorEngine exited unexpectedly for workspace '{launchContext.WorkspaceRoot}'.",
                        await ReadNativeExitCodeSafelyAsync(
                            cancellationToken).ConfigureAwait(false));
                }
            }
        }

        async Task ShutdownNativeAfterFailedStartAsync(
            bool startRequested,
            Task? runtimeMonitorTask,
            CancellationTokenSource? runtimeMonitorCancellation)
        {
            Exception? failure = null;
            runtimeMonitorCancellation?.Cancel();
            if (startRequested)
            {
                failure = await StopNativeSessionAsync().ConfigureAwait(false);
            }

            if (runtimeMonitorTask is not null)
            {
                try
                {
                    await runtimeMonitorTask.WaitAsync(
                        TimeSpan.FromSeconds(10)).ConfigureAwait(false);
                }
                catch (OperationCanceledException)
                    when (runtimeMonitorCancellation?.IsCancellationRequested == true)
                {
                }
                catch (Exception ex)
                {
                    failure = CombineFailures(failure, ex);
                }
                if (!runtimeMonitorTask.IsCompleted)
                {
                    throw failure ?? new TimeoutException(
                        "SailorEngine liveness monitor did not stop after startup failed.");
                }
            }

            var shutdownFailure = await ShutdownNativeSessionAsync(
                stopNative: !startRequested,
                destroyRemoteViewport: false).ConfigureAwait(false);
            if (shutdownFailure is not null)
            {
                failure = CombineFailures(failure, shutdownFailure);
            }
            if (failure is not null)
            {
                throw failure;
            }
        }

        async Task<Exception?> StopNativeSessionAsync()
            => await RequestNativeStopAsync().ConfigureAwait(false);

        async Task<Exception?> RequestNativeStopAsync()
        {
            try
            {
                await protocolClient.StopAsync().ConfigureAwait(false);
                return null;
            }
            catch (Exception stopException)
            {
                try
                {
                    await protocolClient.RequestLocalStopFallbackAsync()
                        .ConfigureAwait(false);
                }
                catch (Exception fallbackException)
                {
                    return new AggregateException(
                        stopException,
                        fallbackException);
                }
                return stopException;
            }
        }

        async Task<Exception?> ShutdownNativeSessionAsync(
            bool stopNative,
            bool destroyRemoteViewport)
        {
            Exception? failure = null;
            if (stopNative)
            {
                var stopFailure =
                    await RequestNativeStopAsync().ConfigureAwait(false);
                if (stopFailure is not null)
                {
                    failure = stopFailure;
                }
            }

            if (destroyRemoteViewport)
            {
                try
                {
                    await protocolClient.DestroyRemoteViewportAsync(
                        SceneViewportId).ConfigureAwait(false);
                }
                catch (Exception ex)
                {
                    failure = failure is null
                        ? ex
                        : new AggregateException(failure, ex);
                }
            }

            try
            {
                await protocolClient.ShutdownAsync().ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                failure = failure is null
                    ? ex
                    : new AggregateException(failure, ex);
                try
                {
                    await protocolClient.CompleteLocalShutdownFallbackAsync()
                        .ConfigureAwait(false);
                }
                catch (Exception fallbackException)
                {
                    failure = new AggregateException(
                        failure,
                        fallbackException);
                }
            }
#if WINDOWS || MACCATALYST
            ResetPlatformInteropState();
#endif
            return failure;
        }

        async Task<string[]?> PullMessagesAsync(
            long generation,
            bool allowStarting = false,
            CancellationToken cancellationToken = default)
        {
            if (!IsGenerationActive(generation, allowStarting))
            {
                return null;
            }

            if (!IsGenerationActive(generation, allowStarting))
            {
                return null;
            }
            var messages = await protocolClient.GetMessagesAsync(
                64,
                cancellationToken).ConfigureAwait(false);

            return messages.Length == 0 ? null : messages;
        }

        async Task<(IReadOnlyList<EditorViewportEvent> Events, long EventEpoch)>
            PullEditorViewportEventsAsync(
            long generation,
            CancellationToken cancellationToken = default)
        {
            var eventEpoch = editorViewportEventEpoch.Current;
            if (!IsGenerationActive(generation))
            {
                return (Array.Empty<EditorViewportEvent>(), eventEpoch);
            }

            IReadOnlyList<ViewportEvent> nativeEvents;
            var parsedEvents = new List<EditorViewportEvent>(MaxEditorViewportEventsPerPoll);
            if (!IsGenerationActive(generation))
            {
                return (Array.Empty<EditorViewportEvent>(), eventEpoch);
            }

            eventEpoch = editorViewportEventEpoch.Current;
            try
            {
                nativeEvents =
                    await protocolClient.PullEditorViewportEventsAsync(
                        MaxEditorViewportEventsPerPoll,
                        cancellationToken).ConfigureAwait(false);
            }
            catch (EngineProtocolException exception)
            {
                Console.WriteLine(
                    $"[EngineService] Failed to poll protocol viewport events: {exception.Message}");
                return (Array.Empty<EditorViewportEvent>(), eventEpoch);
            }

            for (var i = 0; i < nativeEvents.Count; ++i)
            {
                if (EditorViewportEventContract.TryCreate(
                        nativeEvents[i],
                        out var viewportEvent,
                        out var error) &&
                    viewportEvent is not null)
                {
                    parsedEvents.Add(viewportEvent);
                }
                else
                {
                    Console.WriteLine(
                        $"[EngineService] Rejected protocol viewport event {i}: {error}");
                }
            }

            return (
                IsGenerationActive(generation) &&
                    editorViewportEventEpoch.IsCurrent(eventEpoch)
                    ? parsedEvents.ToArray()
                    : Array.Empty<EditorViewportEvent>(),
                eventEpoch);
        }

        async Task<(string SerializedWorld, long Sequence)> SerializeWorldAsync(
            long generation,
            bool allowStarting = false,
            CancellationToken cancellationToken = default)
        {
            if (!IsGenerationActive(generation, allowStarting))
            {
                return (string.Empty, 0);
            }

            if (!IsGenerationActive(generation, allowStarting))
            {
                return (string.Empty, 0);
            }
            var snapshotSequence =
                worldSnapshotPublication.ReserveSequence();
            var serializedWorld =
                await protocolClient.SerializeCurrentWorldAsync(
                    cancellationToken).ConfigureAwait(false);
            return (serializedWorld, snapshotSequence);
        }

        async Task<string> SerializeEditorTypesAsync(
            long generation,
            bool allowStarting = false,
            CancellationToken cancellationToken = default)
        {
            if (!IsGenerationActive(generation, allowStarting))
            {
                return string.Empty;
            }

            if (!IsGenerationActive(generation, allowStarting))
            {
                return string.Empty;
            }
            return await protocolClient.SerializeEditorTypesAsync(
                cancellationToken).ConfigureAwait(false);
        }

        async Task<EditorTypeCacheIdentity> ReadWorkspaceCacheIdentityAsync(
            long generation,
            EngineLaunchContext launchContext,
            bool allowStarting = false,
            CancellationToken cancellationToken = default)
        {
            if (!IsGenerationActive(generation, allowStarting))
            {
                throw new EngineLifecycleException(
                    "The engine generation changed before workspace cache identity could be read.",
                    -1);
            }

            if (!IsGenerationActive(generation, allowStarting))
            {
                throw new EngineLifecycleException(
                    "The engine generation changed before workspace cache identity could be read.",
                    -1);
            }
            var yaml =
                await protocolClient.SerializeWorkspaceCacheIdentityAsync(
                    cancellationToken).ConfigureAwait(false);

            if (string.IsNullOrWhiteSpace(yaml))
            {
                throw new EngineLifecycleException(
                    "SailorEngine did not provide a workspace cache identity.",
                    -1);
            }

            EditorTypeCacheIdentity identity;
            try
            {
                identity = EditorTypeCacheStore.ParseNativeIdentity(yaml);
            }
            catch (Exception ex)
            {
                throw new EngineLifecycleException(
                    $"SailorEngine returned an invalid workspace cache identity: {ex.Message}",
                    -1,
                    ex);
            }

            if (!string.Equals(
                    identity.WorkspaceIdentity,
                    launchContext.WorkspaceIdentity,
                    StringComparison.Ordinal))
            {
                throw new EngineLifecycleException(
                    $"SailorEngine workspace identity mismatch. Expected '{launchContext.WorkspaceIdentity}', received '{identity.WorkspaceIdentity}'.",
                    -1);
            }

            return identity;
        }

        void QueueWorldUpdate(string serializedWorld, long generation, long snapshotSequence)
        {
            if (string.IsNullOrEmpty(serializedWorld) || !IsGenerationActive(generation, allowStarting: true))
            {
                return;
            }

            MainThread.BeginInvokeOnMainThread(() =>
            {
                PublishWorldUpdate(serializedWorld, generation, snapshotSequence, allowStarting: true);
            });
        }

        void PublishWorldUpdate(
            string serializedWorld,
            long generation,
            long snapshotSequence,
            bool allowStarting = false)
        {
            if (string.IsNullOrEmpty(serializedWorld) ||
                !IsGenerationActive(generation, allowStarting) ||
                !worldSnapshotPublication.TryAdvance(snapshotSequence))
            {
                return;
            }

            if (IsGenerationActive(generation, allowStarting))
                OnUpdateCurrentWorldAction?.Invoke(serializedWorld);
        }

        static bool TryParseEditorTypes(
            string yaml,
            out EngineTypes catalog,
            out string error)
        {
            try
            {
                catalog = EngineTypes.FromYaml(yaml);
                if (catalog.Components.Count == 0 && catalog.Enums.Count == 0 && catalog.AssetTypes.Count == 0)
                {
                    error = "The catalog does not contain any reflected or asset types.";
                    catalog = new EngineTypes();
                    return false;
                }

                error = string.Empty;
                return true;
            }
            catch (Exception ex)
            {
                catalog = new EngineTypes();
                error = ex.Message;
                return false;
            }
        }

        void LogEditorTypeCacheInvalidation(string cacheFilePath, string reason)
        {
            var invalidation = editorTypeCacheStore.Invalidate(cacheFilePath);
            Console.WriteLine(invalidation.Succeeded
                ? $"Invalidated editor type cache ({reason}): {cacheFilePath}"
                : invalidation.Diagnostic);
        }

        public Task<bool> CommitChangesAsync(
            InstanceId id,
            string yamlChanges,
            CancellationToken cancellationToken = default)
        {
            var stringId = id?.Value ?? string.Empty;
            return InvokeRunningInteropAsync(
                token => protocolClient.UpdateObjectAsync(
                    stringId,
                    yamlChanges,
                    token),
                invalidateQueuedWorldSnapshots: true,
                cancellationToken);
        }

        public Task<bool> UpdateAssetAsync(
            FileId fileId,
            CancellationToken cancellationToken = default)
        {
            var stringId = fileId?.Value ?? string.Empty;
            return InvokeRunningInteropAsync(
                token => protocolClient.UpdateAssetAsync(stringId, token),
                cancellationToken: cancellationToken);
        }

        public Task<bool> PreviewAudioAssetAsync(
            FileId fileId,
            CancellationToken cancellationToken = default)
        {
            var stringId = fileId?.Value ?? string.Empty;
            return InvokeRunningInteropAsync(
                token => protocolClient.PreviewAudioAssetAsync(stringId, token),
                cancellationToken: cancellationToken);
        }

        public Task<bool> SetAnimatorFloatAsync(
            InstanceId instanceId,
            string name,
            float value,
            CancellationToken cancellationToken = default) =>
            InvokeRunningInteropAsync(
                token => protocolClient.SetAnimatorFloatAsync(
                    instanceId?.Value ?? string.Empty,
                    name,
                    value,
                    token),
                cancellationToken: cancellationToken);

        public Task<bool> SetAnimatorIntAsync(
            InstanceId instanceId,
            string name,
            int value,
            CancellationToken cancellationToken = default) =>
            InvokeRunningInteropAsync(
                token => protocolClient.SetAnimatorIntAsync(
                    instanceId?.Value ?? string.Empty,
                    name,
                    value,
                    token),
                cancellationToken: cancellationToken);

        public Task<bool> SetAnimatorBoolAsync(
            InstanceId instanceId,
            string name,
            bool value,
            CancellationToken cancellationToken = default) =>
            InvokeRunningInteropAsync(
                token => protocolClient.SetAnimatorBoolAsync(
                    instanceId?.Value ?? string.Empty,
                    name,
                    value,
                    token),
                cancellationToken: cancellationToken);

        public Task<bool> SetAnimatorTriggerAsync(
            InstanceId instanceId,
            string name,
            bool reset = false,
            CancellationToken cancellationToken = default) =>
            InvokeRunningInteropAsync(
                token => protocolClient.SetAnimatorTriggerAsync(
                    instanceId?.Value ?? string.Empty,
                    name,
                    reset,
                    token),
                cancellationToken: cancellationToken);

        public async Task<EngineProtocolAnimatorState?> GetAnimatorStateAsync(
            InstanceId instanceId,
            CancellationToken cancellationToken = default)
        {
            if (!IsInteropRunning() || instanceId is null || instanceId.IsEmpty())
            {
                return null;
            }
            return await protocolClient.GetAnimatorStateAsync(
                    instanceId.Value,
                    cancellationToken)
                .ConfigureAwait(false);
        }

        public async Task<bool> RequestAssetReloadAsync(CancellationToken cancellationToken = default)
        {
            var generation = Volatile.Read(ref engineGeneration);
            CancellationToken sessionCancellationToken;
            lock (runLock)
            {
                sessionCancellationToken =
                    activeSession?.PollCancellation.Token ?? default;
            }
            using var operationCancellation =
                CancellationTokenSource.CreateLinkedTokenSource(
                    cancellationToken,
                    sessionCancellationToken);
            var operationCancellationToken =
                operationCancellation.Token;
            var completionSource = new TaskCompletionSource<bool>(
                TaskCreationOptions.RunContinuationsAsynchronously);
            long targetReloadGeneration = 0;

            void HandleCompletion(AssetReloadCompletion completion)
            {
                var targetGeneration = Volatile.Read(
                    ref targetReloadGeneration);
                if (targetGeneration != 0 &&
                    completion.Generation >= (ulong)targetGeneration &&
                    IsGenerationActive(generation))
                {
                    completionSource.TrySetResult(completion.Succeeded);
                }
            }

            OnAssetReloadCompleted += HandleCompletion;
            try
            {
                if (!IsGenerationActive(generation) ||
                    !await protocolClient.RequestAssetReloadAsync(
                        operationCancellationToken).ConfigureAwait(false))
                {
                    return false;
                }

                var reloadState =
                    await protocolClient.GetAssetReloadStateAsync(
                        operationCancellationToken).ConfigureAwait(false);
                if (!reloadState.Available)
                {
                    return false;
                }

                var targetGeneration =
                    checked((long)reloadState.RequestGeneration);
                Volatile.Write(
                    ref targetReloadGeneration,
                    targetGeneration);
                if (reloadState.CompletedGeneration >=
                    (ulong)targetGeneration)
                {
                    return reloadState.SuccessfulGeneration ==
                        reloadState.CompletedGeneration;
                }

                // Completion can occur after the first state snapshot but
                // before targetReloadGeneration is published. An event in
                // that window is intentionally ignored because it cannot be
                // associated with this request yet. Re-read after publishing
                // the target so that such a completion cannot be lost.
                var postPublicationState =
                    await protocolClient.GetAssetReloadStateAsync(
                        operationCancellationToken).ConfigureAwait(false);
                if (!postPublicationState.Available)
                {
                    return false;
                }
                if (postPublicationState.CompletedGeneration >=
                    (ulong)targetGeneration)
                {
                    return postPublicationState.SuccessfulGeneration ==
                        postPublicationState.CompletedGeneration;
                }

                return await completionSource.Task
                    .WaitAsync(
                        TimeSpan.FromSeconds(30),
                        operationCancellationToken)
                    .ConfigureAwait(false);
            }
            catch (TimeoutException)
            {
                return false;
            }
            finally
            {
                OnAssetReloadCompleted -= HandleCompletion;
            }
        }

        public async Task RefreshCurrentWorldAsync(
            CancellationToken cancellationToken = default)
        {
            using var perfScope = EditorPerf.Scope("EngineService.RefreshCurrentWorld");
            var generation = Volatile.Read(ref engineGeneration);
            var snapshot =
                await SerializeWorldAsync(
                    generation,
                    cancellationToken: cancellationToken)
                    .ConfigureAwait(false);
            if (!string.IsNullOrEmpty(snapshot.SerializedWorld))
            {
                if (MainThread.IsMainThread)
                {
                    PublishWorldUpdate(
                        snapshot.SerializedWorld,
                        generation,
                        snapshot.Sequence);
                }
                else
                {
                    await MainThread.InvokeOnMainThreadAsync(() =>
                        PublishWorldUpdate(
                            snapshot.SerializedWorld,
                            generation,
                            snapshot.Sequence));
                }
            }
        }

        public async Task<bool> RefreshCurrentWorldAuthoritativelyAsync(
            CancellationToken cancellationToken = default)
        {
            using var perfScope = EditorPerf.Scope(
                "EngineService.RefreshCurrentWorldAuthoritatively");
            var generation = Volatile.Read(ref engineGeneration);
            var world = MauiProgram.GetService<WorldService>();
            var workspaceEpoch = world.WorkspaceEpoch;
            var snapshot =
                await SerializeWorldAsync(
                    generation,
                    cancellationToken: cancellationToken)
                    .ConfigureAwait(false);
            if (string.IsNullOrEmpty(snapshot.SerializedWorld))
            {
                return false;
            }

            var populated = false;
            void PopulateAuthoritativeSnapshot()
            {
                if (!IsGenerationActive(generation) ||
                    !worldSnapshotPublication.TryAdvance(snapshot.Sequence))
                {
                    return;
                }

                populated = world.TryPopulateWorld(
                    snapshot.SerializedWorld,
                    workspaceEpoch);
            }

            if (MainThread.IsMainThread)
            {
                PopulateAuthoritativeSnapshot();
            }
            else
            {
                await MainThread.InvokeOnMainThreadAsync(
                    PopulateAuthoritativeSnapshot);
            }

            return populated &&
                IsGenerationActive(generation) &&
                world.WorkspaceEpoch == workspaceEpoch;
        }

        async Task<InstanceId?> InvokeCreationInteropAsync(
            Func<CancellationToken, Task<EngineProtocolCreationResult>> interop,
            bool refreshWorld,
            CancellationToken cancellationToken)
        {
            var creationResult = default(EngineProtocolCreationResult);
            if (!await InvokeRunningInteropAsync(async token =>
                {
                    creationResult =
                        await interop(token).ConfigureAwait(false);
                    return creationResult.Succeeded;
                },
                cancellationToken: cancellationToken).ConfigureAwait(false))
            {
                return null;
            }

            if (string.IsNullOrWhiteSpace(creationResult.InstanceId))
            {
                return null;
            }

            var createdInstanceId =
                new InstanceId(creationResult.InstanceId);
            if (refreshWorld)
            {
                await RefreshCurrentWorldAsync(
                    cancellationToken).ConfigureAwait(false);
            }
            return createdInstanceId;
        }

        public async Task<bool> ReparentObjectAsync(
            InstanceId instanceId,
            InstanceId? parentId,
            bool keepWorldTransform = true,
            CancellationToken cancellationToken = default)
        {
            var stringId = instanceId?.Value ?? string.Empty;
            var stringParentId = parentId?.Value ?? string.Empty;
            var result = await InvokeRunningInteropAsync(
                token => protocolClient.ReparentObjectAsync(
                    stringId,
                    stringParentId,
                    keepWorldTransform,
                    token),
                cancellationToken: cancellationToken).ConfigureAwait(false);

            if (result)
            {
                await RefreshCurrentWorldAsync(
                    cancellationToken).ConfigureAwait(false);
            }

            return result;
        }

        public Task<InstanceId?> CreateGameObjectAsync(
            InstanceId? parentId = null,
            InstanceId? preferredInstanceId = null,
            CancellationToken cancellationToken = default)
        {
            var stringParentId = parentId?.Value ?? string.Empty;
            var stringPreferredInstanceId = preferredInstanceId?.Value ?? string.Empty;
            return InvokeCreationInteropAsync(
                token => protocolClient.CreateGameObjectAsync(
                    stringParentId,
                    stringPreferredInstanceId,
                    token),
                refreshWorld: true,
                cancellationToken);
        }

        internal Task<InstanceId?> RequestCreateGameObjectAsync(
            InstanceId? parentId,
            InstanceId preferredInstanceId,
            CancellationToken cancellationToken = default)
            => InvokeCreationInteropAsync(
                token => protocolClient.CreateGameObjectAsync(
                    parentId?.Value ?? string.Empty,
                    preferredInstanceId?.Value ?? string.Empty,
                    token),
                refreshWorld: false,
                cancellationToken);

        internal Task<InstanceId?> RequestCreateModelInstanceAsync(
            FileId modelFileId,
            string name,
            InstanceId? parentId,
            bool createHierarchy,
            Vec4? worldPosition,
            InstanceId preferredInstanceId,
            CancellationToken cancellationToken = default)
            => InvokeCreationInteropAsync(
                token => protocolClient.CreateModelInstanceAsync(
                    modelFileId.Value,
                    name,
                    parentId?.Value ?? string.Empty,
                    createHierarchy,
                    worldPosition is null
                        ? null
                        : new EngineProtocolVector4(
                            worldPosition.X,
                            worldPosition.Y,
                            worldPosition.Z,
                            worldPosition.W),
                    preferredInstanceId.Value,
                    token),
                refreshWorld: false,
                cancellationToken);

        public async Task<Vec4?> TraceViewportRayAsync(
            double normalizedX,
            double normalizedY,
            CancellationToken cancellationToken = default)
        {
            if (!double.IsFinite(normalizedX) ||
                !double.IsFinite(normalizedY) ||
                normalizedX < 0.0 ||
                normalizedX > 1.0 ||
                normalizedY < 0.0 ||
                normalizedY > 1.0)
            {
                return null;
            }

            var resolved = default(EngineProtocolVector4);
            var succeeded = await InvokeRunningInteropAsync(async token =>
                {
                    resolved =
                        await protocolClient.TraceViewportRayAsync(
                            SceneViewportId,
                            (float)normalizedX,
                            (float)normalizedY,
                            token).ConfigureAwait(false);
                    return true;
                },
                cancellationToken: cancellationToken).ConfigureAwait(false);
            return succeeded
                ? new Vec4
                {
                    X = resolved.X,
                    Y = resolved.Y,
                    Z = resolved.Z,
                    W = resolved.W
                }
                : null;
        }

        public async Task<bool> DestroyObjectAsync(
            InstanceId instanceId,
            CancellationToken cancellationToken = default)
        {
            var result = await RequestDestroyObjectAsync(
                instanceId,
                cancellationToken).ConfigureAwait(false);

            if (result)
            {
                await RefreshCurrentWorldAsync(
                    cancellationToken).ConfigureAwait(false);
            }

            return result;
        }

        internal Task<bool> RequestDestroyObjectAsync(
            InstanceId instanceId,
            CancellationToken cancellationToken = default)
        {
            var stringId = instanceId?.Value ?? string.Empty;
            return InvokeRunningInteropAsync(
                token => protocolClient.DestroyObjectAsync(
                    stringId,
                    token),
                cancellationToken: cancellationToken);
        }

        public async Task<bool> ResetComponentToDefaultsAsync(
            InstanceId instanceId,
            CancellationToken cancellationToken = default)
        {
            var stringId = instanceId?.Value ?? string.Empty;
            var result = await InvokeRunningInteropAsync(
                token => protocolClient.ResetComponentToDefaultsAsync(
                    stringId,
                    token),
                cancellationToken: cancellationToken).ConfigureAwait(false);

            if (result)
            {
                await RefreshCurrentWorldAsync(
                    cancellationToken).ConfigureAwait(false);
            }

            return result;
        }

        public Task<InstanceId?> AddComponentAsync(
            InstanceId instanceId,
            string componentTypeName,
            InstanceId? preferredInstanceId = null,
            CancellationToken cancellationToken = default)
        {
            var stringId = instanceId?.Value ?? string.Empty;
            var stringPreferredInstanceId = preferredInstanceId?.Value ?? string.Empty;
            return InvokeCreationInteropAsync(
                token => protocolClient.AddComponentAsync(
                    stringId,
                    componentTypeName ?? string.Empty,
                    stringPreferredInstanceId,
                    token),
                refreshWorld: true,
                cancellationToken);
        }

        internal Task<InstanceId?> RequestAddComponentAsync(
            InstanceId instanceId,
            string componentTypeName,
            InstanceId preferredInstanceId,
            CancellationToken cancellationToken = default)
            => InvokeCreationInteropAsync(
                token => protocolClient.AddComponentAsync(
                    instanceId?.Value ?? string.Empty,
                    componentTypeName ?? string.Empty,
                    preferredInstanceId?.Value ?? string.Empty,
                    token),
                refreshWorld: false,
                cancellationToken);

        public async Task<bool> RemoveComponentAsync(
            InstanceId instanceId,
            CancellationToken cancellationToken = default)
        {
            var stringId = instanceId?.Value ?? string.Empty;
            var result = await InvokeRunningInteropAsync(
                token => protocolClient.RemoveComponentAsync(
                    stringId,
                    token),
                cancellationToken: cancellationToken).ConfigureAwait(false);

            if (result)
            {
                await RefreshCurrentWorldAsync(
                    cancellationToken).ConfigureAwait(false);
            }

            return result;
        }

        public async Task<bool> InstantiatePrefabAsync(
            FileId prefabId,
            InstanceId? parentId = null,
            CancellationToken cancellationToken = default)
        {
            var stringFileId = prefabId?.Value ?? string.Empty;
            var stringParentId = parentId?.Value ?? string.Empty;
            var result = await InvokeRunningInteropAsync(
                token => protocolClient.InstantiatePrefabAsync(
                    stringFileId,
                    stringParentId,
                    token),
                cancellationToken: cancellationToken).ConfigureAwait(false);

            if (result)
            {
                await RefreshCurrentWorldAsync(
                    cancellationToken).ConfigureAwait(false);
            }

            return result;
        }

        public Task<InstanceId?> InstantiatePrefabInstanceAsync(
            FileId prefabId,
            InstanceId? parentId = null,
            Vec4? worldPosition = null,
            CancellationToken cancellationToken = default)
        {
            var position = worldPosition is null
                ? (EngineProtocolVector4?)null
                : new EngineProtocolVector4(
                    worldPosition.X,
                    worldPosition.Y,
                    worldPosition.Z,
                    worldPosition.W);
            return InvokeCreationInteropAsync(
                token => protocolClient.InstantiatePrefabInstanceAsync(
                    prefabId?.Value ?? string.Empty,
                    parentId?.Value ?? string.Empty,
                    position,
                    token),
                refreshWorld: true,
                cancellationToken);
        }

        public Task<bool> FocusEditorCameraAsync(
            InstanceId instanceId,
            CancellationToken cancellationToken = default)
            => InvokeRunningInteropAsync(
                token => protocolClient.FocusEditorCameraAsync(
                    SceneViewportId,
                    instanceId?.Value ?? string.Empty,
                    token),
                cancellationToken: cancellationToken);

        public async Task<bool> SetPrefabLinkAsync(
            InstanceId instanceId,
            FileId prefabId,
            CancellationToken cancellationToken = default)
        {
            var result = await InvokeRunningInteropAsync(
                token => protocolClient.SetPrefabLinkAsync(
                    instanceId?.Value ?? string.Empty,
                    prefabId?.Value ?? string.Empty,
                    token),
                cancellationToken: cancellationToken).ConfigureAwait(false);
            if (result)
            {
                await RefreshCurrentWorldAsync(cancellationToken)
                    .ConfigureAwait(false);
            }
            return result;
        }

        public async Task<bool> BreakPrefabLinkAsync(
            InstanceId instanceId,
            CancellationToken cancellationToken = default)
        {
            var result = await InvokeRunningInteropAsync(
                token => protocolClient.BreakPrefabLinkAsync(
                    instanceId?.Value ?? string.Empty,
                    token),
                cancellationToken: cancellationToken).ConfigureAwait(false);
            if (result)
            {
                await RefreshCurrentWorldAsync(cancellationToken)
                    .ConfigureAwait(false);
            }
            return result;
        }

        public Task<bool> SetViewportToolStateAsync(
            EditorViewportTransformOperation operation,
            EditorViewportTransformSpace space,
            CancellationToken cancellationToken = default)
            => InvokeRunningInteropAsync(
                token => protocolClient.SetViewportToolStateAsync(
                    SceneViewportId,
                    ToProtocolOperation(operation),
                    ToProtocolSpace(space),
                    token),
                cancellationToken: cancellationToken);

        public async Task<SceneViewportToolState?> GetViewportToolStateAsync(
            CancellationToken cancellationToken = default)
        {
            var state = default(EngineProtocolViewportToolState);
            var succeeded = await InvokeRunningInteropAsync(async token =>
                {
                    state = await protocolClient.GetViewportToolStateAsync(
                        SceneViewportId,
                        token).ConfigureAwait(false);
                    return true;
                },
                cancellationToken: cancellationToken).ConfigureAwait(false);
            return succeeded
                ? new SceneViewportToolState(
                    FromProtocolOperation(state.Operation),
                    FromProtocolSpace(state.Space))
                : null;
        }

        static ViewportTransformOperation ToProtocolOperation(
            EditorViewportTransformOperation operation)
            => operation switch
            {
                EditorViewportTransformOperation.Select =>
                    ViewportTransformOperation.Select,
                EditorViewportTransformOperation.Translate =>
                    ViewportTransformOperation.Translate,
                EditorViewportTransformOperation.Rotate =>
                    ViewportTransformOperation.Rotate,
                EditorViewportTransformOperation.Scale =>
                    ViewportTransformOperation.Scale,
                _ => throw new ArgumentOutOfRangeException(nameof(operation))
            };

        static ViewportTransformSpace ToProtocolSpace(
            EditorViewportTransformSpace space)
            => space switch
            {
                EditorViewportTransformSpace.World =>
                    ViewportTransformSpace.World,
                EditorViewportTransformSpace.Local =>
                    ViewportTransformSpace.Local,
                _ => throw new ArgumentOutOfRangeException(nameof(space))
            };

        static EditorViewportTransformOperation FromProtocolOperation(
            ViewportTransformOperation operation)
            => operation switch
            {
                ViewportTransformOperation.Select =>
                    EditorViewportTransformOperation.Select,
                ViewportTransformOperation.Translate =>
                    EditorViewportTransformOperation.Translate,
                ViewportTransformOperation.Rotate =>
                    EditorViewportTransformOperation.Rotate,
                ViewportTransformOperation.Scale =>
                    EditorViewportTransformOperation.Scale,
                _ => throw new EngineProtocolException(
                    $"Unsupported viewport transform operation '{operation}'.")
            };

        static EditorViewportTransformSpace FromProtocolSpace(
            ViewportTransformSpace space)
            => space switch
            {
                ViewportTransformSpace.World =>
                    EditorViewportTransformSpace.World,
                ViewportTransformSpace.Local =>
                    EditorViewportTransformSpace.Local,
                _ => throw new EngineProtocolException(
                    $"Unsupported viewport transform space '{space}'.")
            };

        public Task<InstanceId?> InstantiatePrefabFromYamlAsync(
            string prefabYaml,
            InstanceId? parentId = null,
            CancellationToken cancellationToken = default)
        {
            if (string.IsNullOrWhiteSpace(prefabYaml))
            {
                return Task.FromResult<InstanceId?>(null);
            }

            return InvokeCreationInteropAsync(
                token => protocolClient.InstantiatePrefabFromYamlAsync(
                    prefabYaml,
                    parentId?.Value ?? string.Empty,
                    token),
                refreshWorld: true,
                cancellationToken);
        }

        public Task<InstanceId?> InstantiatePrefabFromYamlStrictAsync(
            string prefabYaml,
            InstanceId? parentId,
            CancellationToken cancellationToken = default)
        {
            if (string.IsNullOrWhiteSpace(prefabYaml))
            {
                return Task.FromResult<InstanceId?>(null);
            }

            return InvokeCreationInteropAsync(
                token => protocolClient.InstantiatePrefabFromYamlStrictAsync(
                    prefabYaml,
                    parentId?.Value ?? string.Empty,
                    token),
                refreshWorld: true,
                cancellationToken);
        }

        internal Task<InstanceId?> RequestInstantiatePrefabFromYamlStrictAsync(
            string prefabYaml,
            InstanceId? parentId,
            CancellationToken cancellationToken = default)
        {
            if (string.IsNullOrWhiteSpace(prefabYaml))
            {
                return Task.FromResult<InstanceId?>(null);
            }

            var stringParentId = parentId?.Value ?? string.Empty;
            return InvokeCreationInteropAsync(
                token => protocolClient.InstantiatePrefabFromYamlStrictAsync(
                    prefabYaml,
                    stringParentId,
                    token),
                refreshWorld: false,
                cancellationToken);
        }

        public async Task<bool> LoadWorldAsync(
            FileId worldId,
            CancellationToken cancellationToken = default)
        {
            var stringFileId = worldId?.Value ?? string.Empty;
            var result = await InvokeRunningInteropAsync(async token =>
            {
                if (!await protocolClient.LoadEditorWorldAsync(
                        stringFileId,
                        token).ConfigureAwait(false))
                {
                    return false;
                }

                editorViewportEventEpoch.Advance();
                return true;
            }, cancellationToken: cancellationToken).ConfigureAwait(false);

            if (result)
            {
                PublishEditorSimulationState(false);
                await RefreshCurrentWorldAsync(
                    cancellationToken).ConfigureAwait(false);
            }
            else
            {
                PublishEditorSimulationState(
                    await GetEditorSimulationStateAsync(
                        cancellationToken).ConfigureAwait(false));
            }

            return result;
        }

        public async Task<bool> CreateWorldAsync(
            CancellationToken cancellationToken = default)
        {
            var result = await InvokeRunningInteropAsync(async token =>
            {
                if (!await protocolClient.CreateEditorWorldAsync(
                        token).ConfigureAwait(false))
                {
                    return false;
                }

                editorViewportEventEpoch.Advance();
                return true;
            }, cancellationToken: cancellationToken).ConfigureAwait(false);

            if (result)
            {
                PublishEditorSimulationState(false);
                await RefreshCurrentWorldAsync(
                    cancellationToken).ConfigureAwait(false);
            }
            else
            {
                PublishEditorSimulationState(
                    await GetEditorSimulationStateAsync(
                        cancellationToken).ConfigureAwait(false));
            }

            return result;
        }

        public async Task<bool> SetEditorSimulationAsync(
            bool enabled,
            CancellationToken cancellationToken = default)
        {
            var result = await InvokeRunningInteropAsync(
                token => protocolClient.SetEditorSimulationAsync(
                    enabled,
                    token),
                cancellationToken: cancellationToken).ConfigureAwait(false);
            if (result)
                PublishEditorSimulationState(enabled);
            return result;
        }

        public Task<bool> GetEditorSimulationStateAsync(
            CancellationToken cancellationToken = default)
            => InvokeRunningInteropAsync(
                protocolClient.GetEditorSimulationStateAsync,
                cancellationToken: cancellationToken);

        void PublishEditorSimulationState(bool enabled)
        {
            void Publish() => OnEditorSimulationStateChanged(enabled);
            if (MainThread.IsMainThread)
                Publish();
            else
                MainThread.BeginInvokeOnMainThread(Publish);
        }

        public async Task<string> SerializeCurrentWorldAsync(
            CancellationToken cancellationToken = default)
        {
            var generation = Volatile.Read(ref engineGeneration);
            var result =
                await SerializeWorldAsync(
                    generation,
                    cancellationToken: cancellationToken)
                    .ConfigureAwait(false);
            return result.SerializedWorld;
        }

        public Task<bool> UpdateEditorSelectionAsync(
            IEnumerable<InstanceId?> selection,
            CancellationToken cancellationToken = default)
        {
            var instanceIds = BuildEditorSelectionIds(selection);
            return InvokeRunningInteropAsync(
                token => protocolClient.SetEditorSelectionAsync(
                    instanceIds,
                    token),
                cancellationToken: cancellationToken);
        }

        public static string[] BuildEditorSelectionIds(IEnumerable<InstanceId?> selection)
            => selection?
                .Where(id => id is not null && !id.IsEmpty())
                .Select(id => id!.Value)
                .ToArray() ?? Array.Empty<string>();

        public async Task<bool> ExportPathTracedImageAsync(
            string outputPath,
            InstanceId? targetInstance = null,
            uint height = 720,
            uint samplesPerPixel = 64,
            uint maxBounces = 4,
            CancellationToken cancellationToken = default)
        {
            string strInstanceId = targetInstance?.Value ?? string.Empty;
            EngineSession? session;
            CancellationToken backgroundCancellationToken;
            lock (runLock)
            {
                session = State == EngineLifecycleState.Running
                    ? activeSession
                    : null;
                backgroundCancellationToken =
                    session?.BackgroundCancellation.Token ?? default;
            }
            if (session is null)
            {
                return false;
            }

            try
            {
                using var linkedCancellation =
                    CancellationTokenSource.CreateLinkedTokenSource(
                        backgroundCancellationToken,
                        cancellationToken);
                return await protocolClient.RenderPathTracedImageAsync(
                    outputPath,
                    strInstanceId,
                    height,
                    samplesPerPixel,
                    maxBounces,
                    linkedCancellation.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
                when (backgroundCancellationToken.IsCancellationRequested)
            {
                return false;
            }
        }

        public void RunWorld(string world, bool bDebug)
            => RunWorld(world, bDebug, GetLaunchContext());

        public void RunWorld(string world, bool bDebug, EngineLaunchContext launchContext)
        {
            try
            {
                ProcessStartInfo startInfo = new ProcessStartInfo
                {
                    FileName = bDebug ? PathToEngineExecDebug : PathToEngineExec,
                    WorkingDirectory = EngineWorkingDirectory,
                    UseShellExecute = false
                };

                foreach (var argument in launchContext.BuildArguments(world))
                    startInfo.ArgumentList.Add(argument);

                Process process = new Process { StartInfo = startInfo };
                Console.WriteLine($"Starting SailorEngine process with workspace: {launchContext.WorkspaceRoot}");
                process.Start();
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Cannot run SailorEngine process: {ex.Message}");
            }
        }

        Task BeginDisposeAsync()
        {
            lock (disposeGate)
            {
                if (disposeTask is not null)
                {
                    return disposeTask;
                }

                Interlocked.Exchange(ref disposeState, 1);
                disposeCancellation.Cancel();
                if (ReferenceEquals(currentInstance, this))
                {
                    currentInstance = null;
                }
                disposeTask = Task.Run(DisposeCoreAsync);
                return disposeTask;
            }
        }

        async Task DisposeCoreAsync()
        {
            Exception? disposalFailure = null;
            var protocolClientDisposed = false;
            void DisposeProtocolClientOnce()
            {
                if (protocolClientDisposed)
                {
                    return;
                }
                protocolClientDisposed = true;
                try
                {
                    protocolClient.Dispose();
                }
                catch (Exception exception)
                {
                    disposalFailure = CombineFailures(
                        disposalFailure,
                        exception);
                }
            }

            async Task DrainProtocolClientAsync()
            {
                DisposeProtocolClientOnce();
                try
                {
                    await protocolClient.DisposeAsync().ConfigureAwait(false);
                }
                catch (Exception exception)
                {
                    disposalFailure = CombineFailures(
                        disposalFailure,
                        exception);
                }
            }

            try
            {
                try
                {
                    using var orderlyStopCancellation =
                        new CancellationTokenSource(
                            DisposeOrderlyStopTimeout);
                    await StopAsync(orderlyStopCancellation.Token)
                        .ConfigureAwait(false);
                }
                catch (Exception exception)
                {
                    disposalFailure = exception;
                }

                Task? completionTask;
                lock (runLock)
                {
                    activeSession?.PollCancellation.Cancel();
                    activeSession?.BackgroundCancellation.Cancel();
                    activeSession?.RuntimeMonitorCancellation.Cancel();
                    completionTask = activeSession?.CompletionTask;
                }
                if (completionTask is not null &&
                    !completionTask.IsCompleted)
                {
                    try
                    {
                        await completionTask.WaitAsync(
                            DisposeCompletionTimeout).ConfigureAwait(false);
                    }
                    catch (TimeoutException exception)
                    {
                        disposalFailure = CombineFailures(
                            disposalFailure,
                            exception);
                        DisposeProtocolClientOnce();
                        try
                        {
                            await completionTask.WaitAsync(
                                DisposeAbortDrainTimeout).ConfigureAwait(false);
                        }
                        catch (Exception drainException)
                        {
                            disposalFailure = CombineFailures(
                                disposalFailure,
                                drainException);
                        }
                    }
                    catch (Exception exception)
                    {
                        disposalFailure = CombineFailures(
                            disposalFailure,
                            exception);
                    }
                }

                try
                {
                    var platformQueue = CapturePlatformInteropQueue();
                    await platformQueue.WaitAsync(
                        DisposePlatformQueueTimeout).ConfigureAwait(false);
                }
                catch (TimeoutException exception)
                {
                    disposalFailure = CombineFailures(
                        disposalFailure,
                        exception);
                    DisposeProtocolClientOnce();
                }
                catch (Exception exception)
                {
                    disposalFailure = CombineFailures(
                        disposalFailure,
                        exception);
                }

                await DrainProtocolClientAsync().ConfigureAwait(false);
            }
            finally
            {
                disposeCancellation.Dispose();
            }

            if (disposalFailure is null)
            {
                if (State != EngineLifecycleState.Faulted)
                {
                    SetLifecycleState(EngineLifecycleState.Stopped);
                }
            }
            else
            {
                lock (runLock)
                {
                    lastFailure ??= disposalFailure;
                }
                SetLifecycleState(EngineLifecycleState.Faulted);
                Console.WriteLine(
                    $"[EngineService] Native transport disposal failed: {disposalFailure.Message}");
            }
        }

        public void Dispose()
            => _ = BeginDisposeAsync();

        public ValueTask DisposeAsync()
            => new(BeginDisposeAsync());
    }
}
