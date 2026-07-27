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

        readonly object interopLock = new();
        readonly object platformInteropQueueLock = new();
        readonly object sceneViewportStateLock = new();
        readonly object runLock = new();
        readonly object disposeGate = new();
        readonly EngineProtocolClient protocolClient;
        readonly SemaphoreSlim lifecycleGate = new(1, 1);
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
#else
                return Path.Combine(EngineWorkingDirectory, "Binaries", "Debug", "SailorEngine-Debug");
#endif
            }
        }

        public string PathToEngineExec
        {
            get
            {
#if WINDOWS
                return Path.Combine(EngineWorkingDirectory, "SailorEngine-Release.exe");
#else
                return Path.Combine(EngineWorkingDirectory, "Binaries", "Release", "SailorEngine-Release");
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

        bool IsInteropRunningUnderLock() => State == EngineLifecycleState.Running;

        bool TryGetAssetReloadState(
            long generation,
            out ulong requestGeneration,
            out ulong completedGeneration,
            out ulong successfulGeneration,
            CancellationToken cancellationToken = default)
        {
            requestGeneration = 0;
            completedGeneration = 0;
            successfulGeneration = 0;
            lock (interopLock)
            {
                if (!IsGenerationActive(generation))
                {
                    return false;
                }

                var state = protocolClient.GetAssetReloadState(
                    cancellationToken);
                requestGeneration = state.RequestGeneration;
                completedGeneration = state.CompletedGeneration;
                successfulGeneration = state.SuccessfulGeneration;
                return state.Available;
            }
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

        bool InvokeRunningInterop(Func<bool> action, bool invalidateQueuedWorldSnapshots = false)
        {
            lock (interopLock)
            {
                if (!IsInteropRunningUnderLock() || !action())
                    return false;

                if (invalidateQueuedWorldSnapshots)
                {
                    var mutationSequence = worldSnapshotPublication.ReserveSequence();
                    worldSnapshotPublication.TryAdvance(mutationSequence);
                }

                return true;
            }
        }

        bool QueuePlatformInterop(Func<bool> action)
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
                platformInteropQueue = platformInteropQueue.ContinueWith(
                    _ =>
                    {
                        try
                        {
                            if (IsGenerationActive(generation))
                            {
                                action();
                            }
                        }
                        catch (Exception ex)
                        {
                            Console.WriteLine(
                                $"[EngineService] Platform interop command failed: {ex.Message}");
                        }
                        finally
                        {
                            Interlocked.Decrement(
                                ref pendingPlatformInteropCommands);
                        }
                    },
                    CancellationToken.None,
                    TaskContinuationOptions.DenyChildAttach,
                    TaskScheduler.Default);
            }
            return true;
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

        Task<bool> IsEditorViewportEventCurrentAsync(
            EditorManagedMutationKind kind,
            string instanceId,
            ulong managedMutationRevision)
            => Task.Run(() =>
            {
                lock (interopLock)
                {
                    return IsInteropRunningUnderLock() &&
                        EditorViewportMutationOrder.IsCurrent(
                            managedMutationRevision,
                            protocolClient.GetEditorManagedMutationRevision(
                                (uint)kind,
                                instanceId));
                }
            });

        public void InvalidateQueuedWorldSnapshots()
        {
            lock (interopLock)
            {
                var mutationSequence = worldSnapshotPublication.ReserveSequence();
                worldSnapshotPublication.TryAdvance(mutationSequence);
            }
        }

        public static void ShowMainWindow(bool bShow)
        {
            var instance = Volatile.Read(ref currentInstance);
            if (instance is null || !instance.IsRunning)
            {
                return;
            }

#if WINDOWS || MACCATALYST
            instance.QueuePlatformInterop(() =>
            {
                instance.protocolClient.ShowMainWindow(bShow);
                return true;
            });
#else
            lock (instance.interopLock)
            {
                if (instance.IsInteropRunningUnderLock())
                {
                    instance.protocolClient.ShowMainWindow(bShow);
                }
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

        public void BindMacRemoteViewportHost(ulong viewportId, nint hostHandle)
        {
#if MACCATALYST
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

            if (!QueuePlatformInterop(() =>
            {
                if (protocolClient.SetRemoteViewportMacHostHandle(
                        viewportId,
                        2u,
                        (ulong)hostHandle))
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
                    update =>
                        protocolClient.UpsertRemoteViewport(
                            update.ViewportId,
                            (uint)update.Rect.X,
                            (uint)update.Rect.Y,
                            (uint)update.Rect.Width,
                            (uint)update.Rect.Height,
                            update.Visible,
                            update.Focused));
#else
            return false;
#endif
        }

        bool TryRefreshSceneRemoteViewport(
            long generation,
            CancellationToken cancellationToken = default)
        {
#if WINDOWS
            SceneViewportStateSnapshot<Rect> viewportState;
            lock (sceneViewportStateLock)
            {
                viewportState = sceneViewportState.Capture();
            }
            lock (interopLock)
            {
                if (!IsGenerationActive(generation))
                {
                    return false;
                }
                if (TryUpdateRemoteViewportUnderLock(
                    SceneViewportId,
                    viewportState.Rect,
                    viewportState.Visible,
                    viewportState.Focused,
                    cancellationToken))
                {
                    return true;
                }

                if (!viewportState.Rect.IsEmpty)
                {
                    protocolClient.SetViewport(
                        (uint)viewportState.Rect.X,
                        (uint)viewportState.Rect.Y,
                        (uint)viewportState.Rect.Width,
                        (uint)viewportState.Rect.Height,
                        cancellationToken);
                }

                return false;
            }
#else
            return false;
#endif
        }

        bool TryUpdateRemoteViewportUnderLock(
            ulong viewportId,
            Rect rect,
            bool visible,
            bool focused,
            CancellationToken cancellationToken = default)
        {
#if WINDOWS || MACCATALYST
            return IsInteropRunningUnderLock() &&
                !rect.IsEmpty &&
                protocolClient.UpsertRemoteViewport(
                    viewportId,
                    (uint)rect.X,
                    (uint)rect.Y,
                    (uint)rect.Width,
                    (uint)rect.Height,
                    visible,
                    focused,
                    cancellationToken);
#else
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
                update =>
                {
                    protocolClient.SetViewport(
                        (uint)update.X,
                        (uint)update.Y,
                        (uint)update.Width,
                        (uint)update.Height);
                    return true;
                });
#else
            lock (sceneViewportStateLock)
            {
                sceneViewportState.RememberRect(rect);
            }
            lock (interopLock)
            {
                if (IsInteropRunningUnderLock())
                {
                    protocolClient.SetViewport(
                        (uint)rect.X,
                        (uint)rect.Y,
                        (uint)rect.Width,
                        (uint)rect.Height);
                }
            }
#endif
        }

        public void SetEditorRenderTargetSize(uint width, uint height)
        {
#if MACCATALYST
            renderTargetUpdate.Enqueue(
                (Math.Max(width, 1u), Math.Max(height, 1u)),
                QueuePlatformInterop,
                size =>
                {
                    protocolClient.SetEditorRenderTargetSize(
                        size.Width,
                        size.Height);
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
            QueuePlatformInterop(() =>
            {
                var destroyed =
                    protocolClient.DestroyRemoteViewport(viewportId);
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
            QueuePlatformInterop(() =>
                protocolClient.RetryRemoteViewport(viewportId));
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
            bool SendInput(RemoteViewportInput value)
                => protocolClient.SendRemoteViewportInput(
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
                    value.Captured);

            return kind == RemoteViewportInputKind.PointerMove
                ? pointerMoves.Enqueue(
                    viewportId,
                    input,
                    QueuePlatformInterop,
                    SendInput)
                : QueuePlatformInterop(() => SendInput(input));
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
                id =>
                {
                    var state = (RemoteViewportSessionState)
                        protocolClient.GetRemoteViewportState(id);
                    var diagnostics =
                        protocolClient.GetRemoteViewportDiagnostics(id);
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
                await MainThread.InvokeOnMainThreadAsync(() =>
                {
                    lock (interopLock)
                    {
                        protocolClient.Initialize(args);
                    }
                });
#else
                lock (interopLock)
                {
                    protocolClient.Initialize(args);
                }
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
                await Task.Run(
                    () => protocolClient.Start(monitorCancellation.Token),
                    CancellationToken.None).ConfigureAwait(false);
                await WaitForEngineMainThreadAsync(
                    startCancellationToken).ConfigureAwait(false);
                runtimeMonitorTask = MonitorEngineLifetimeAsync(
                    generation,
                    launchContext,
                    monitorCancellation.Token);

                var initializationExitCode = ReadNativeExitCode();
                if (initializationExitCode != 0)
                {
                    throw new EngineLifecycleException(
                        $"SailorEngine initialization failed for workspace '{launchContext.WorkspaceRoot}' with exit code {initializationExitCode}.",
                        initializationExitCode);
                }

                var editorTypeCacheIdentity = ReadWorkspaceCacheIdentity(
                    generation,
                    launchContext,
                    allowStarting: true,
                    cancellationToken: startCancellationToken);
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
                string serializedEditorTypes = SerializeEditorTypes(
                    generation,
                    allowStarting: true,
                    cancellationToken: startCancellationToken);
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
                serializedWorld = await MainThread.InvokeOnMainThreadAsync(
                    () => SerializeWorld(
                        generation,
                        out serializedWorldSequence,
                        allowStarting: true,
                        cancellationToken: startCancellationToken));
#else
                serializedWorld = SerializeWorld(
                    generation,
                    out serializedWorldSequence,
                    allowStarting: true,
                    cancellationToken: startCancellationToken);
#endif
                QueueWorldUpdate(serializedWorld, generation, serializedWorldSequence);

                var bootstrapMessages = PullMessages(
                    generation,
                    allowStarting: true,
                    cancellationToken: startCancellationToken);
                if (bootstrapMessages is not null)
                {
                    PublishConsoleMessages(bootstrapMessages, generation);
                }

                var pollCancellation = new CancellationTokenSource();
                var backgroundCancellation = new CancellationTokenSource();
                var pollTasks = new List<Task>();
#if !MACCATALYST
                pollTasks.Add(RunPeriodicTaskAsync(() =>
                {
                    TryRefreshSceneRemoteViewport(
                        generation,
                        pollCancellation.Token);

                    return Task.CompletedTask;
                }, 500, 100, pollCancellation.Token, generation));
#endif
                pollTasks.Add(RunPeriodicTaskAsync(() =>
                {
                    var messages = PullMessages(
                        generation,
                        cancellationToken: pollCancellation.Token);
                    if (messages is not null)
                    {
                        PublishConsoleMessages(messages, generation);
                    }
                    return Task.CompletedTask;
                }, 300, 500, pollCancellation.Token, generation));

                pollTasks.Add(RunPeriodicTaskAsync(() =>
                {
                    var viewportEvents = PullEditorViewportEvents(
                        generation,
                        out var eventEpoch,
                        pollCancellation.Token);
                    if (viewportEvents.Count > 0)
                    {
                        PublishEditorViewportEvents(viewportEvents, generation, eventEpoch);
                    }
                    return Task.CompletedTask;
                }, 33, 33, pollCancellation.Token, generation));

                ulong lastAssetReloadCompletion = 0;
                pollTasks.Add(RunPeriodicTaskAsync(() =>
                {
                    if (TryGetAssetReloadState(
                            generation,
                            out var requestedReloadGeneration,
                            out var completedReloadGeneration,
                            out var successfulReloadGeneration,
                            pollCancellation.Token) &&
                        completedReloadGeneration > lastAssetReloadCompletion &&
                        completedReloadGeneration <= requestedReloadGeneration)
                    {
                        lastAssetReloadCompletion = completedReloadGeneration;
                        PublishAssetReloadCompletion(
                            new AssetReloadCompletion(
                                completedReloadGeneration,
                                successfulReloadGeneration == completedReloadGeneration),
                            generation);
                    }

                    return Task.CompletedTask;
                }, 100, 100, pollCancellation.Token, generation));

                pollTasks.Add(RunPeriodicTaskAsync(() =>
                {
                    var serializedWorld = SerializeWorld(
                        generation,
                        out var serializedWorldSequence,
                        cancellationToken: pollCancellation.Token);
                    QueueWorldUpdate(serializedWorld, generation, serializedWorldSequence);
                    return Task.CompletedTask;
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
                        exitCode = ReadNativeExitCode();
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
                    stopFailure = RequestNativeStop();
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
                    protocolClient.RequestLocalStopFallback();
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
                    failure ??= new EngineLifecycleException(
                        $"SailorEngine exited unexpectedly for workspace '{session.LaunchContext.WorkspaceRoot}'.",
                        ReadNativeExitCodeSafely(
                            exitCodeCancellation.Token));
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
                    exitCode = ReadNativeExitCode(
                        exitCodeCancellation.Token);
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

        int ReadNativeExitCode(
            CancellationToken cancellationToken = default)
        {
            lock (interopLock)
            {
                return protocolClient.GetExitCode(
                    cancellationToken);
            }
        }

        int ReadNativeExitCodeSafely(
            CancellationToken cancellationToken = default)
        {
            try
            {
                return ReadNativeExitCode(cancellationToken);
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
                if (!protocolClient.IsEngineRunning(
                        cancellationToken))
                {
                    throw new EngineLifecycleException(
                        "SailorEngine exited before its main thread became ready.",
                        ReadNativeExitCodeSafely(cancellationToken));
                }
                if (protocolClient.IsEngineMainThreadReady(
                        cancellationToken))
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

                bool isRunning;
                lock (interopLock)
                {
                    isRunning = protocolClient.IsEngineRunning(
                        cancellationToken);
                }
                if (!isRunning)
                {
                    throw new EngineLifecycleException(
                        $"SailorEngine exited unexpectedly for workspace '{launchContext.WorkspaceRoot}'.",
                        ReadNativeExitCodeSafely(cancellationToken));
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

        Task<Exception?> StopNativeSessionAsync()
            => Task.Run(RequestNativeStop);

        Exception? RequestNativeStop()
        {
            try
            {
                protocolClient.Stop();
                return null;
            }
            catch (Exception stopException)
            {
                try
                {
                    protocolClient.RequestLocalStopFallback();
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

        Task<Exception?> ShutdownNativeSessionAsync(bool stopNative, bool destroyRemoteViewport)
            => Task.Run(() =>
                ShutdownNativeSessionUnderLock(stopNative, destroyRemoteViewport));

        Exception? ShutdownNativeSessionUnderLock(bool stopNative, bool destroyRemoteViewport)
        {
            lock (interopLock)
            {
                Exception? failure = null;
                if (stopNative)
                {
                    var stopFailure = RequestNativeStop();
                    if (stopFailure is not null)
                    {
                        failure = stopFailure;
                    }
                }

                if (destroyRemoteViewport)
                {
                    try
                    {
                        protocolClient.DestroyRemoteViewport(SceneViewportId);
                    }
                    catch (Exception ex)
                    {
                        failure = failure is null ? ex : new AggregateException(failure, ex);
                    }
                }

                try
                {
                    protocolClient.Shutdown();
                }
                catch (Exception ex)
                {
                    failure = failure is null
                        ? ex
                        : new AggregateException(failure, ex);
                    try
                    {
                        protocolClient.CompleteLocalShutdownFallback();
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
        }

        string[]? PullMessages(
            long generation,
            bool allowStarting = false,
            CancellationToken cancellationToken = default)
        {
            if (!IsGenerationActive(generation, allowStarting))
            {
                return null;
            }

            string[] messages;
            lock (interopLock)
            {
                if (!IsGenerationActive(generation, allowStarting))
                {
                    return null;
                }
                messages = protocolClient.GetMessages(
                    64,
                    cancellationToken);
            }

            return messages.Length == 0 ? null : messages;
        }

        IReadOnlyList<EditorViewportEvent> PullEditorViewportEvents(
            long generation,
            out long eventEpoch,
            CancellationToken cancellationToken = default)
        {
            eventEpoch = editorViewportEventEpoch.Current;
            if (!IsGenerationActive(generation))
            {
                return Array.Empty<EditorViewportEvent>();
            }

            IReadOnlyList<ViewportEvent> nativeEvents;
            var parsedEvents = new List<EditorViewportEvent>(MaxEditorViewportEventsPerPoll);
            lock (interopLock)
            {
                if (!IsGenerationActive(generation))
                {
                    return Array.Empty<EditorViewportEvent>();
                }

                eventEpoch = editorViewportEventEpoch.Current;
                try
                {
                    nativeEvents = protocolClient.PullEditorViewportEvents(
                        MaxEditorViewportEventsPerPoll,
                        cancellationToken);
                }
                catch (EngineProtocolException exception)
                {
                    Console.WriteLine(
                        $"[EngineService] Failed to poll protocol viewport events: {exception.Message}");
                    return Array.Empty<EditorViewportEvent>();
                }
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

            return IsGenerationActive(generation) && editorViewportEventEpoch.IsCurrent(eventEpoch)
                ? parsedEvents.ToArray()
                : Array.Empty<EditorViewportEvent>();
        }

        string SerializeWorld(
            long generation,
            out long snapshotSequence,
            bool allowStarting = false,
            CancellationToken cancellationToken = default)
        {
            snapshotSequence = 0;
            if (!IsGenerationActive(generation, allowStarting))
            {
                return string.Empty;
            }

            lock (interopLock)
            {
                if (!IsGenerationActive(generation, allowStarting))
                {
                    return string.Empty;
                }
                snapshotSequence = worldSnapshotPublication.ReserveSequence();
                return protocolClient.SerializeCurrentWorld(
                    cancellationToken);
            }
        }

        string SerializeEditorTypes(
            long generation,
            bool allowStarting = false,
            CancellationToken cancellationToken = default)
        {
            if (!IsGenerationActive(generation, allowStarting))
            {
                return string.Empty;
            }

            lock (interopLock)
            {
                if (!IsGenerationActive(generation, allowStarting))
                {
                    return string.Empty;
                }
                return protocolClient.SerializeEditorTypes(
                    cancellationToken);
            }
        }

        EditorTypeCacheIdentity ReadWorkspaceCacheIdentity(
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

            string yaml;
            lock (interopLock)
            {
                if (!IsGenerationActive(generation, allowStarting))
                {
                    throw new EngineLifecycleException(
                        "The engine generation changed before workspace cache identity could be read.",
                        -1);
                }
                yaml = protocolClient.SerializeWorkspaceCacheIdentity(
                    cancellationToken);
            }

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

        public bool CommitChanges(InstanceId id, string yamlChanges)
        {
            var stringId = id.Value.ToString();
            return InvokeRunningInterop(
                () => protocolClient.UpdateObject(stringId, yamlChanges),
                invalidateQueuedWorldSnapshots: true);
        }

        public bool RequestAssetReload()
        {
            return InvokeRunningInterop(
                () => protocolClient.RequestAssetReload());
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
                var targetGeneration = Volatile.Read(ref targetReloadGeneration);
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
                var initialState = await Task.Run(
                    () =>
                    {
                        lock (interopLock)
                        {
                            if (!IsGenerationActive(generation) ||
                                !protocolClient.RequestAssetReload(
                                    operationCancellationToken))
                            {
                                return (Accepted: false, Completed: false, Succeeded: false);
                            }

                            var reloadState =
                                protocolClient.GetAssetReloadState(
                                    operationCancellationToken);
                            if (!reloadState.Available)
                            {
                                return (Accepted: false, Completed: false, Succeeded: false);
                            }

                            var targetGeneration =
                                checked((long)reloadState.RequestGeneration);
                            Volatile.Write(
                                ref targetReloadGeneration,
                                targetGeneration);
                            var completed =
                                reloadState.CompletedGeneration >=
                                (ulong)targetGeneration;
                            return (
                                Accepted: true,
                                Completed: completed,
                                Succeeded: completed &&
                                    reloadState.SuccessfulGeneration ==
                                    reloadState.CompletedGeneration);
                        }
                    },
                    operationCancellationToken).ConfigureAwait(false);
                if (!initialState.Accepted)
                {
                    return false;
                }
                if (initialState.Completed)
                {
                    return initialState.Succeeded;
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

        public void RefreshCurrentWorld()
        {
            using var perfScope = EditorPerf.Scope("EngineService.RefreshCurrentWorld");
            var generation = Volatile.Read(ref engineGeneration);
            string serializedWorld = SerializeWorld(generation, out var serializedWorldSequence);
            if (!string.IsNullOrEmpty(serializedWorld))
            {
                if (MainThread.IsMainThread)
                {
                    PublishWorldUpdate(serializedWorld, generation, serializedWorldSequence);
                }
                else
                {
                    MainThread.InvokeOnMainThreadAsync(() =>
                    {
                        PublishWorldUpdate(serializedWorld, generation, serializedWorldSequence);
                    }).GetAwaiter().GetResult();
                }
            }
        }

        bool InvokeCreationInterop(
            Func<EngineProtocolCreationResult> interop,
            out InstanceId createdInstanceId)
        {
            createdInstanceId = null;
            var creationResult = default(EngineProtocolCreationResult);
            if (!InvokeRunningInterop(() =>
                {
                    creationResult = interop();
                    return creationResult.Succeeded;
                }))
            {
                return false;
            }

            if (string.IsNullOrWhiteSpace(creationResult.InstanceId))
            {
                return false;
            }

            createdInstanceId = new InstanceId(creationResult.InstanceId);
            RefreshCurrentWorld();
            return true;
        }

        public bool ReparentObject(InstanceId instanceId, InstanceId parentId, bool keepWorldTransform = true)
        {
            var stringId = instanceId?.Value ?? string.Empty;
            var stringParentId = parentId?.Value ?? string.Empty;
            bool result = InvokeRunningInterop(() =>
                protocolClient.ReparentObject(
                    stringId,
                    stringParentId,
                    keepWorldTransform));

            if (result)
            {
                RefreshCurrentWorld();
            }

            return result;
        }

        public bool CreateGameObject(InstanceId parentId = null)
            => CreateGameObject(parentId, null, out _);

        public bool CreateGameObject(InstanceId parentId, InstanceId preferredInstanceId, out InstanceId createdInstanceId)
        {
            var stringParentId = parentId?.Value ?? string.Empty;
            var stringPreferredInstanceId = preferredInstanceId?.Value ?? string.Empty;
            return InvokeCreationInterop(
                () => protocolClient.CreateGameObject(
                    stringParentId,
                    stringPreferredInstanceId),
                out createdInstanceId);
        }

        public bool DestroyObject(InstanceId instanceId)
        {
            var stringId = instanceId?.Value ?? string.Empty;
            bool result = InvokeRunningInterop(() => protocolClient.DestroyObject(stringId));

            if (result)
            {
                RefreshCurrentWorld();
            }

            return result;
        }

        public bool ResetComponentToDefaults(InstanceId instanceId)
        {
            var stringId = instanceId?.Value ?? string.Empty;
            bool result = InvokeRunningInterop(() =>
                protocolClient.ResetComponentToDefaults(stringId));

            if (result)
            {
                RefreshCurrentWorld();
            }

            return result;
        }

        public bool AddComponent(InstanceId instanceId, string componentTypeName)
            => AddComponent(instanceId, componentTypeName, null, out _);

        public bool AddComponent(
            InstanceId instanceId,
            string componentTypeName,
            InstanceId preferredInstanceId,
            out InstanceId createdInstanceId)
        {
            var stringId = instanceId?.Value ?? string.Empty;
            var stringPreferredInstanceId = preferredInstanceId?.Value ?? string.Empty;
            return InvokeCreationInterop(
                () => protocolClient.AddComponent(
                    stringId,
                    componentTypeName ?? string.Empty,
                    stringPreferredInstanceId),
                out createdInstanceId);
        }

        public bool RemoveComponent(InstanceId instanceId)
        {
            var stringId = instanceId?.Value ?? string.Empty;
            bool result = InvokeRunningInterop(() => protocolClient.RemoveComponent(stringId));

            if (result)
            {
                RefreshCurrentWorld();
            }

            return result;
        }

        public bool InstantiatePrefab(FileId prefabId, InstanceId parentId = null)
        {
            var stringFileId = prefabId?.Value ?? string.Empty;
            var stringParentId = parentId?.Value ?? string.Empty;
            bool result = InvokeRunningInterop(() =>
                protocolClient.InstantiatePrefab(stringFileId, stringParentId));

            if (result)
            {
                RefreshCurrentWorld();
            }

            return result;
        }

        public bool InstantiatePrefabFromYaml(string prefabYaml, InstanceId parentId = null)
        {
            if (string.IsNullOrWhiteSpace(prefabYaml))
            {
                return false;
            }

            var stringParentId = parentId?.Value ?? string.Empty;
            bool result = InvokeRunningInterop(() =>
                protocolClient.InstantiatePrefabFromYaml(prefabYaml, stringParentId));

            if (result)
            {
                RefreshCurrentWorld();
            }

            return result;
        }

        public bool LoadWorld(FileId worldId)
        {
            var stringFileId = worldId?.Value ?? string.Empty;
            bool result = InvokeRunningInterop(() =>
            {
                if (!protocolClient.LoadEditorWorld(stringFileId))
                {
                    return false;
                }

                editorViewportEventEpoch.Advance();
                return true;
            });

            if (result)
            {
                RefreshCurrentWorld();
            }

            return result;
        }

        public bool CreateWorld()
        {
            bool result = InvokeRunningInterop(() =>
            {
                if (!protocolClient.CreateEditorWorld())
                {
                    return false;
                }

                editorViewportEventEpoch.Advance();
                return true;
            });

            if (result)
            {
                RefreshCurrentWorld();
            }

            return result;
        }

        public string SerializeCurrentWorld()
        {
            var generation = Volatile.Read(ref engineGeneration);
            return SerializeWorld(generation, out _);
        }

        public bool UpdateEditorSelection(IEnumerable<InstanceId?> selection)
        {
            var instanceIds = BuildEditorSelectionIds(selection);
            return InvokeRunningInterop(() =>
                protocolClient.SetEditorSelection(instanceIds));
        }

        public static string[] BuildEditorSelectionIds(IEnumerable<InstanceId?> selection)
            => selection?
                .Where(id => id is not null && !id.IsEmpty())
                .Select(id => id!.Value)
                .ToArray() ?? Array.Empty<string>();

        public bool ExportPathTracedImage(string outputPath, InstanceId targetInstance = null, uint height = 720, uint samplesPerPixel = 64, uint maxBounces = 4)
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
                return protocolClient.RenderPathTracedImage(
                    outputPath,
                    strInstanceId,
                    height,
                    samplesPerPixel,
                    maxBounces,
                    backgroundCancellationToken);
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
