using SailorEngine;
using System.Runtime.InteropServices;
using System.Diagnostics;
using YamlDotNet.Serialization;
using SailorEditor.Utility;
using SailorEditor.Workspace;
using System.Threading;
using System.Globalization;

namespace SailorEngine
{
    public class EngineAppInterop
    {
#if MACCATALYST
        static EngineAppInterop()
        {
            NativeLibrary.SetDllImportResolver(typeof(EngineAppInterop).Assembly, static (libraryName, assembly, searchPath) =>
            {
                if (libraryName != EngineLibrary)
                {
                    return IntPtr.Zero;
                }

                var bundledPath = Path.Combine(AppContext.BaseDirectory, "..", "Resources", $"{EngineLibrary}.dylib");
                bundledPath = Path.GetFullPath(bundledPath);

                return File.Exists(bundledPath)
                    ? NativeLibrary.Load(bundledPath)
                    : IntPtr.Zero;
            });
        }
#endif
#if MACCATALYST
#if DEBUG
        const string EngineLibrary = "Sailor-Debug";
#else
        const string EngineLibrary = "Sailor-Release";
#endif
#elif DEBUG
        const string EngineLibrary = "../../../../../Sailor-RelWithDebInfo.dll";
#else
        const string EngineLibrary = "../../../../../Sailor-Release.dll";
#endif

        [DllImport(EngineLibrary, EntryPoint = "Initialize", ExactSpelling = true, CallingConvention = CallingConvention.Cdecl)]
        static extern void InitializeNative([In] nint[] commandLineArgs, int num);

        public static void Initialize(string[] commandLineArgs, int num)
        {
            var nativeArguments = Utf8InteropArguments.Allocate(commandLineArgs, num);
            try
            {
                InitializeNative(nativeArguments, num);
            }
            finally
            {
                Utf8InteropArguments.Free(nativeArguments);
            }
        }

        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern void Start();
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern void Stop();
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern void Shutdown();
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern int GetExitCode();
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern uint GetMessages(nint[] messages, uint num);
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern uint SerializeCurrentWorld(nint[] yamlNode);
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern uint SerializeEditorTypes(nint[] yamlNode);
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern uint SerializeWorkspaceCacheIdentity(nint[] yamlNode);
        [return: MarshalAs(UnmanagedType.I1)]
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern bool LoadEditorWorld(string strFileId);
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern void SetViewport(uint windowPosX, uint windowPosY, uint width, uint height);
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern void SetEditorRenderTargetSize(uint width, uint height);
        [return: MarshalAs(UnmanagedType.I1)]
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern bool UpsertRemoteViewport(ulong viewportId, uint windowPosX, uint windowPosY, uint width, uint height, [MarshalAs(UnmanagedType.I1)] bool visible, [MarshalAs(UnmanagedType.I1)] bool focused);
        [return: MarshalAs(UnmanagedType.I1)]
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern bool DestroyRemoteViewport(ulong viewportId);
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern uint GetRemoteViewportState(ulong viewportId);
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern uint GetRemoteViewportDiagnostics(ulong viewportId, nint[] diagnostics);
        [return: MarshalAs(UnmanagedType.I1)]
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern bool RetryRemoteViewport(ulong viewportId);
        [return: MarshalAs(UnmanagedType.I1)]
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern bool SetRemoteViewportMacHostHandle(ulong viewportId, uint hostHandleKind, ulong hostHandleValue);
        [return: MarshalAs(UnmanagedType.I1)]
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern bool SendRemoteViewportInput(ulong viewportId, uint kind, float pointerX, float pointerY, float wheelDeltaX, float wheelDeltaY, uint keyCode, uint button, uint modifiers, [MarshalAs(UnmanagedType.I1)] bool pressed, [MarshalAs(UnmanagedType.I1)] bool focused, [MarshalAs(UnmanagedType.I1)] bool captured);
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern void FreeInteropString(nint text);
        [return: MarshalAs(UnmanagedType.I1)]
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern bool UpdateObject(string strInstanceId, string strYamlNode);
        [return: MarshalAs(UnmanagedType.I1)]
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern bool ReparentObject(string strInstanceId, string strParentInstanceId, [MarshalAs(UnmanagedType.I1)] bool bKeepWorldTransform);
        [return: MarshalAs(UnmanagedType.I1)]
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern bool CreateGameObject(string strParentInstanceId);
        [return: MarshalAs(UnmanagedType.I1)]
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern bool DestroyObject(string strInstanceId);
        [return: MarshalAs(UnmanagedType.I1)]
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern bool ResetComponentToDefaults(string strInstanceId);
        [return: MarshalAs(UnmanagedType.I1)]
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern bool AddComponent(string strInstanceId, string strComponentTypeName);
        [return: MarshalAs(UnmanagedType.I1)]
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern bool RemoveComponent(string strInstanceId);
        [return: MarshalAs(UnmanagedType.I1)]
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern bool InstantiatePrefab(string strFileId, string strParentInstanceId);
        [return: MarshalAs(UnmanagedType.I1)]
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern bool SetEditorSelection(string strSelectionYaml);
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern void ShowMainWindow([MarshalAs(UnmanagedType.I1)] bool bShow);
        [return: MarshalAs(UnmanagedType.I1)]
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern bool RenderPathTracedImage(string strOutputPath, string strInstanceId, uint height, uint samplesPerPixel, uint maxBounces);
    }
}

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

    internal class EngineService
    {
        sealed class EngineSession
        {
            public required long Generation { get; init; }
            public required EngineLaunchContext LaunchContext { get; init; }
            public required CancellationTokenSource PollCancellation { get; init; }
            public required Task NativeRunTask { get; init; }
            public required IReadOnlyList<Task> PollTasks { get; init; }
            public Task CompletionTask { get; set; } = Task.CompletedTask;
        }

        readonly record struct ConsoleMessage(long Generation, string Text);

        public const ulong SceneViewportId = 1;

        readonly object interopLock = new();
        readonly object runLock = new();
        readonly SemaphoreSlim lifecycleGate = new(1, 1);
        readonly RingBufferedBatcher<ConsoleMessage> consoleMessages = new(MaxBufferedConsoleMessages);
        EngineTypes editorTypes = new();
        int consoleDispatchScheduled = 0;
        int lifecycleState = (int)EngineLifecycleState.Stopped;
        long engineGeneration;
#if MACCATALYST
        readonly Dictionary<ulong, (long Generation, nint Handle)> appliedMacRemoteViewportHosts = [];
#endif
        EngineSession? activeSession;
        EngineLaunchContext? activeLaunchContext;
        int lastExitCode;
        Exception? lastFailure;
        static EngineService? currentInstance;
        const int MaxBufferedConsoleMessages = 1000;

        readonly string repoRoot = ResolveRepoRoot();
        readonly WorkspaceLifecycleService workspaceLifecycle;
        readonly EditorTypeCacheStore editorTypeCacheStore = new();

        public EngineService(WorkspaceLifecycleService workspaceLifecycle)
        {
            this.workspaceLifecycle = workspaceLifecycle;
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

        public Task NativeRunTask
        {
            get
            {
                lock (runLock)
                {
                    return activeSession?.NativeRunTask ?? Task.CompletedTask;
                }
            }
        }

        public event Action<EngineLifecycleState> OnLifecycleStateChanged = delegate { };

        public string EngineContentDirectory => Path.Combine(repoRoot, "Content");

#if MACCATALYST
        public string EngineCacheDirectory
        {
            get
            {
                var localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
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

        public Rect Viewport { get; set; } = new Rect(0, 0, 1024, 768);

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

        bool InvokeRunningInterop(Func<bool> action)
        {
            lock (interopLock)
            {
                return IsInteropRunningUnderLock() && action();
            }
        }

        public static void ShowMainWindow(bool bShow)
        {
            var instance = Volatile.Read(ref currentInstance);
            if (instance is null || !instance.IsRunning)
            {
                return;
            }

            lock (instance.interopLock)
            {
                if (instance.IsInteropRunningUnderLock())
                {
                    EngineAppInterop.ShowMainWindow(bShow);
                }
            }
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
            lock (interopLock)
            {
                var generation = Volatile.Read(ref engineGeneration);
                if (!IsInteropRunningUnderLock())
                {
                    appliedMacRemoteViewportHosts.Remove(viewportId);
                    return;
                }

                if (appliedMacRemoteViewportHosts.TryGetValue(viewportId, out var applied) &&
                    applied.Generation == generation &&
                    applied.Handle == hostHandle)
                {
                    return;
                }

                if (EngineAppInterop.SetRemoteViewportMacHostHandle(viewportId, 2u, (ulong)hostHandle))
                {
                    appliedMacRemoteViewportHosts[viewportId] = (generation, hostHandle);
                }
                else
                {
                    appliedMacRemoteViewportHosts.Remove(viewportId);
                }
            }
#endif
        }

        public bool TryUpdateRemoteViewport(ulong viewportId, Rect rect, bool visible, bool focused)
        {
#if WINDOWS || MACCATALYST
            if (rect.IsEmpty || !IsRunning)
            {
                return false;
            }

            lock (interopLock)
            {
                return IsInteropRunningUnderLock() &&
                    EngineAppInterop.UpsertRemoteViewport(viewportId, (uint)rect.X, (uint)rect.Y, (uint)rect.Width, (uint)rect.Height, visible, focused);
            }
#else
            return false;
#endif
        }

        public void SetViewport(Rect rect)
        {
            if (rect.IsEmpty || !IsRunning)
            {
                return;
            }

            lock (interopLock)
            {
                if (IsInteropRunningUnderLock())
                {
                    EngineAppInterop.SetViewport((uint)rect.X, (uint)rect.Y, (uint)rect.Width, (uint)rect.Height);
                }
            }
        }

        public void SetEditorRenderTargetSize(uint width, uint height)
        {
#if MACCATALYST
            lock (interopLock)
            {
                if (IsInteropRunningUnderLock())
                {
                    EngineAppInterop.SetEditorRenderTargetSize(Math.Max(width, 1u), Math.Max(height, 1u));
                }
            }
#endif
        }

        public void DestroyRemoteViewport(ulong viewportId)
        {
#if WINDOWS || MACCATALYST
            lock (interopLock)
            {
                if (IsInteropRunningUnderLock())
                {
                    EngineAppInterop.DestroyRemoteViewport(viewportId);
                }
            }
#endif
        }

        public RemoteViewportSessionState GetRemoteViewportState(ulong viewportId)
        {
#if WINDOWS || MACCATALYST
            lock (interopLock)
            {
                return IsInteropRunningUnderLock()
                    ? (RemoteViewportSessionState)EngineAppInterop.GetRemoteViewportState(viewportId)
                    : RemoteViewportSessionState.Disposed;
            }
#else
            return RemoteViewportSessionState.Created;
#endif
        }

        public void RetryRemoteViewport(ulong viewportId)
        {
#if WINDOWS || MACCATALYST
            lock (interopLock)
            {
                if (IsInteropRunningUnderLock())
                {
                    EngineAppInterop.RetryRemoteViewport(viewportId);
                }
            }
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
            lock (interopLock)
            {
                return IsInteropRunningUnderLock() &&
                    EngineAppInterop.SendRemoteViewportInput(viewportId, (uint)kind, pointerX, pointerY, wheelDeltaX, wheelDeltaY, keyCode, button, (uint)modifiers, pressed, focused, captured);
            }
#else
            return false;
#endif
        }

        public string GetRemoteViewportDiagnostics(ulong viewportId)
        {
#if WINDOWS || MACCATALYST
            lock (interopLock)
            {
                if (!IsInteropRunningUnderLock())
                {
                    return string.Empty;
                }

                nint[] textPtr = new nint[1];
                uint length = EngineAppInterop.GetRemoteViewportDiagnostics(viewportId, textPtr);
                if (length == 0 || textPtr[0] == nint.Zero)
                {
                    return string.Empty;
                }

                try
                {
                    return Marshal.PtrToStringAnsi(textPtr[0], (int)length) ?? string.Empty;
                }
                finally
                {
                    EngineAppInterop.FreeInteropString(textPtr[0]);
                }
            }
#else
            return string.Empty;
#endif
        }

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
            while (true)
            {
                await lifecycleGate.WaitAsync(cancellationToken).ConfigureAwait(false);
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
                    await stoppingTask.WaitAsync(cancellationToken).ConfigureAwait(false);
                }
            }

            var initialized = false;
            var generation = 0L;
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
                cancellationToken.ThrowIfCancellationRequested();
                generation = Interlocked.Increment(ref engineGeneration);
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
                        EngineAppInterop.Initialize(args, args.Length);
                    }
                });
#else
                lock (interopLock)
                {
                    EngineAppInterop.Initialize(args, args.Length);
                }
#endif
                initialized = true;

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
                    allowStarting: true);
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
                string serializedEditorTypes = SerializeEditorTypes(generation, allowStarting: true);
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

                string serializedWorld = SerializeWorld(generation, allowStarting: true);
                QueueWorldUpdate(serializedWorld, generation);

                var bootstrapMessages = PullMessages(generation, allowStarting: true);
                if (bootstrapMessages is not null)
                {
                    PublishConsoleMessages(bootstrapMessages, generation);
                }

                var pollCancellation = new CancellationTokenSource();
                var pollTasks = new List<Task>();
#if !MACCATALYST
                pollTasks.Add(RunPeriodicTaskAsync(() =>
                {
                    if (!IsGenerationActive(generation))
                    {
                        return Task.CompletedTask;
                    }

                    if (!TryUpdateRemoteViewport(SceneViewportId, Viewport, visible: true, focused: false))
                    {
                        lock (interopLock)
                        {
                            if (IsGenerationActive(generation))
                            {
                                EngineAppInterop.SetViewport((uint)Viewport.X, (uint)Viewport.Y, (uint)Viewport.Width, (uint)Viewport.Height);
                            }
                        }
                    }

                    return Task.CompletedTask;
                }, 500, 100, pollCancellation.Token, generation));
#endif
                pollTasks.Add(RunPeriodicTaskAsync(() =>
                {
                    var messages = PullMessages(generation);
                    if (messages is not null)
                    {
                        PublishConsoleMessages(messages, generation);
                    }
                    return Task.CompletedTask;
                }, 300, 500, pollCancellation.Token, generation));

                pollTasks.Add(RunPeriodicTaskAsync(() =>
                {
                    QueueWorldUpdate(SerializeWorld(generation), generation);
                    return Task.CompletedTask;
                }, 1500, 0, pollCancellation.Token, generation));

                var nativeRunTask = Task.Run(EngineAppInterop.Start, CancellationToken.None);
                var session = new EngineSession
                {
                    Generation = generation,
                    LaunchContext = launchContext,
                    PollCancellation = pollCancellation,
                    NativeRunTask = nativeRunTask,
                    PollTasks = pollTasks
                };

                lock (runLock)
                {
                    activeSession = session;
                }
                SetLifecycleState(EngineLifecycleState.Running);
                session.CompletionTask = CompleteSessionAsync(session);
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
                        await ShutdownNativeAfterFailedStartAsync().ConfigureAwait(false);
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
                if (exitCode == 0)
                {
                    exitCode = -1;
                }

                Volatile.Write(ref lastExitCode, exitCode);
                Interlocked.Increment(ref engineGeneration);
                lock (runLock)
                {
                    activeLaunchContext = null;
                    activeSession = null;
                    lastFailure = failure;
                }
                SetLifecycleState(EngineLifecycleState.Faulted);
                Volatile.Write(ref editorTypes, new EngineTypes());

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
            Exception? stopFailure = null;
            try
            {
                lock (runLock)
                {
                    completionTask = activeSession?.CompletionTask;
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
                    lock (runLock)
                    {
                        activeSession?.PollCancellation.Cancel();
                    }

                    lock (interopLock)
                    {
                        try
                        {
                            EngineAppInterop.Stop();
                        }
                        catch (Exception ex)
                        {
                            stopFailure = ex;
                        }
                    }
                }
            }
            finally
            {
                lifecycleGate.Release();
            }

            // Teardown is deliberately non-cancellable once native shutdown starts.
            await completionTask.ConfigureAwait(false);
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
                await session.NativeRunTask.ConfigureAwait(false);
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

                if (State == EngineLifecycleState.Running)
                {
                    failure = new EngineLifecycleException(
                        $"SailorEngine exited unexpectedly for workspace '{session.LaunchContext.WorkspaceRoot}'.",
                        ReadNativeExitCodeSafely());
                    SetLifecycleState(EngineLifecycleState.Stopping);
                    Interlocked.Increment(ref engineGeneration);
                }
                session.PollCancellation.Cancel();
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
                    exitCode = ReadNativeExitCode();
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
                    "Editor.world",
                    extraArguments).ToArray();
            });
#else
            var extraArguments = new[] { "--noconsole", "--editor" }
                .Concat(commandLineArgs ?? Array.Empty<string>());
            return launchContext.BuildInteropArguments(
                "SailorEditor",
                "Editor.world",
                extraArguments).ToArray();
#endif
        }

        int ReadNativeExitCode()
        {
            lock (interopLock)
            {
                return EngineAppInterop.GetExitCode();
            }
        }

        int ReadNativeExitCodeSafely()
        {
            try
            {
                return ReadNativeExitCode();
            }
            catch
            {
                return -1;
            }
        }

        static Exception CombineFailures(Exception? current, Exception next)
            => current is null ? next : new AggregateException(current, next);

        async Task ShutdownNativeAfterFailedStartAsync()
        {
            var failure = await ShutdownNativeSessionAsync(
                stopNative: true,
                destroyRemoteViewport: false).ConfigureAwait(false);
            if (failure is not null)
            {
                throw failure;
            }
        }

        Task<Exception?> ShutdownNativeSessionAsync(bool stopNative, bool destroyRemoteViewport)
        {
#if WINDOWS || MACCATALYST
            return MainThread.InvokeOnMainThreadAsync(() =>
                ShutdownNativeSessionUnderLock(stopNative, destroyRemoteViewport));
#else
            return Task.FromResult(ShutdownNativeSessionUnderLock(stopNative, destroyRemoteViewport));
#endif
        }

        Exception? ShutdownNativeSessionUnderLock(bool stopNative, bool destroyRemoteViewport)
        {
            lock (interopLock)
            {
                Exception? failure = null;
                if (stopNative)
                {
                    try
                    {
                        EngineAppInterop.Stop();
                    }
                    catch (Exception ex)
                    {
                        failure = ex;
                    }
                }

                if (destroyRemoteViewport)
                {
                    try
                    {
                        EngineAppInterop.DestroyRemoteViewport(SceneViewportId);
                    }
                    catch (Exception ex)
                    {
                        failure = failure is null ? ex : new AggregateException(failure, ex);
                    }
                }

                try
                {
                    EngineAppInterop.Shutdown();
                }
                catch (Exception ex)
                {
                    failure = failure is null
                        ? ex
                        : new AggregateException(failure, ex);
                }

#if MACCATALYST
                appliedMacRemoteViewportHosts.Clear();
#endif
                return failure;
            }
        }

        string[]? PullMessages(long generation, bool allowStarting = false)
        {
            if (!IsGenerationActive(generation, allowStarting))
            {
                return null;
            }

            uint numMessages = 64;
            nint[] messagesPtrs = new nint[numMessages];

            uint actualNumMessages;
            lock (interopLock)
            {
                if (!IsGenerationActive(generation, allowStarting))
                {
                    return null;
                }
                actualNumMessages = EngineAppInterop.GetMessages(messagesPtrs, numMessages);
            }

            if (actualNumMessages == 0)
                return null;

            string[] messages = new string[actualNumMessages];
            for (int i = 0; i < actualNumMessages; i++)
            {
                messages[i] = Marshal.PtrToStringAnsi(messagesPtrs[i]);
                EngineAppInterop.FreeInteropString(messagesPtrs[i]);
            }

            return messages;
        }

        string SerializeWorld(long generation, bool allowStarting = false)
        {
            if (!IsGenerationActive(generation, allowStarting))
            {
                return string.Empty;
            }

            nint[] yamlNodeChar = new nint[1];
            uint numChars;
            lock (interopLock)
            {
                if (!IsGenerationActive(generation, allowStarting))
                {
                    return string.Empty;
                }
                numChars = EngineAppInterop.SerializeCurrentWorld(yamlNodeChar);
            }

            if (numChars == 0)
                return string.Empty;

            try
            {
                return Marshal.PtrToStringAnsi(yamlNodeChar[0], (int)numChars) ?? string.Empty;
            }
            finally
            {
                EngineAppInterop.FreeInteropString(yamlNodeChar[0]);
            }
        }

        string SerializeEditorTypes(long generation, bool allowStarting = false)
        {
            if (!IsGenerationActive(generation, allowStarting))
            {
                return string.Empty;
            }

            nint[] yamlNodeChar = new nint[1];
            uint numChars;
            lock (interopLock)
            {
                if (!IsGenerationActive(generation, allowStarting))
                {
                    return string.Empty;
                }
                numChars = EngineAppInterop.SerializeEditorTypes(yamlNodeChar);
            }

            if (numChars == 0)
                return string.Empty;

            try
            {
                return Marshal.PtrToStringAnsi(yamlNodeChar[0], (int)numChars) ?? string.Empty;
            }
            finally
            {
                EngineAppInterop.FreeInteropString(yamlNodeChar[0]);
            }
        }

        EditorTypeCacheIdentity ReadWorkspaceCacheIdentity(
            long generation,
            EngineLaunchContext launchContext,
            bool allowStarting = false)
        {
            if (!IsGenerationActive(generation, allowStarting))
            {
                throw new EngineLifecycleException(
                    "The engine generation changed before workspace cache identity could be read.",
                    -1);
            }

            nint[] yamlNodeChar = new nint[1];
            uint numChars;
            lock (interopLock)
            {
                if (!IsGenerationActive(generation, allowStarting))
                {
                    throw new EngineLifecycleException(
                        "The engine generation changed before workspace cache identity could be read.",
                        -1);
                }
                numChars = EngineAppInterop.SerializeWorkspaceCacheIdentity(yamlNodeChar);
            }

            if (numChars == 0 || yamlNodeChar[0] == nint.Zero)
            {
                throw new EngineLifecycleException(
                    "SailorEngine did not provide a workspace cache identity.",
                    -1);
            }

            string yaml;
            try
            {
                yaml = Marshal.PtrToStringUTF8(yamlNodeChar[0], (int)numChars) ?? string.Empty;
            }
            finally
            {
                EngineAppInterop.FreeInteropString(yamlNodeChar[0]);
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

        void QueueWorldUpdate(string serializedWorld, long generation)
        {
            if (string.IsNullOrEmpty(serializedWorld) || !IsGenerationActive(generation, allowStarting: true))
            {
                return;
            }

            MainThread.BeginInvokeOnMainThread(() =>
            {
                if (IsGenerationActive(generation, allowStarting: true))
                {
                    OnUpdateCurrentWorldAction?.Invoke(serializedWorld);
                }
            });
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
            return InvokeRunningInterop(() => EngineAppInterop.UpdateObject(stringId, yamlChanges));
        }

        public void RefreshCurrentWorld()
        {
            using var perfScope = EditorPerf.Scope("EngineService.RefreshCurrentWorld");
            var generation = Volatile.Read(ref engineGeneration);
            string serializedWorld = SerializeWorld(generation);
            if (!string.IsNullOrEmpty(serializedWorld))
            {
                if (MainThread.IsMainThread)
                {
                    if (IsGenerationActive(generation))
                    {
                        OnUpdateCurrentWorldAction?.Invoke(serializedWorld);
                    }
                }
                else
                {
                    QueueWorldUpdate(serializedWorld, generation);
                }
            }
        }

        public bool ReparentObject(InstanceId instanceId, InstanceId parentId, bool keepWorldTransform = true)
        {
            var stringId = instanceId?.Value ?? string.Empty;
            var stringParentId = parentId?.Value ?? string.Empty;
            bool result = InvokeRunningInterop(() => EngineAppInterop.ReparentObject(stringId, stringParentId, keepWorldTransform));

            if (result)
            {
                RefreshCurrentWorld();
            }

            return result;
        }

        public bool CreateGameObject(InstanceId parentId = null)
        {
            var stringParentId = parentId?.Value ?? string.Empty;
            bool result = InvokeRunningInterop(() => EngineAppInterop.CreateGameObject(stringParentId));

            if (result)
            {
                RefreshCurrentWorld();
            }

            return result;
        }

        public bool DestroyObject(InstanceId instanceId)
        {
            var stringId = instanceId?.Value ?? string.Empty;
            bool result = InvokeRunningInterop(() => EngineAppInterop.DestroyObject(stringId));

            if (result)
            {
                RefreshCurrentWorld();
            }

            return result;
        }

        public bool ResetComponentToDefaults(InstanceId instanceId)
        {
            var stringId = instanceId?.Value ?? string.Empty;
            bool result = InvokeRunningInterop(() => EngineAppInterop.ResetComponentToDefaults(stringId));

            if (result)
            {
                RefreshCurrentWorld();
            }

            return result;
        }

        public bool AddComponent(InstanceId instanceId, string componentTypeName)
        {
            var stringId = instanceId?.Value ?? string.Empty;
            bool result = InvokeRunningInterop(() => EngineAppInterop.AddComponent(stringId, componentTypeName ?? string.Empty));

            if (result)
            {
                RefreshCurrentWorld();
            }

            return result;
        }

        public bool RemoveComponent(InstanceId instanceId)
        {
            var stringId = instanceId?.Value ?? string.Empty;
            bool result = InvokeRunningInterop(() => EngineAppInterop.RemoveComponent(stringId));

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
            bool result = InvokeRunningInterop(() => EngineAppInterop.InstantiatePrefab(stringFileId, stringParentId));

            if (result)
            {
                RefreshCurrentWorld();
            }

            return result;
        }

        public bool LoadWorld(FileId worldId)
        {
            var stringFileId = worldId?.Value ?? string.Empty;
            bool result = InvokeRunningInterop(() => EngineAppInterop.LoadEditorWorld(stringFileId));

            if (result)
            {
                RefreshCurrentWorld();
            }

            return result;
        }

        public bool UpdateEditorSelection(IEnumerable<InstanceId?> selection)
        {
            var yaml = BuildEditorSelectionYaml(selection);

            return InvokeRunningInterop(() => EngineAppInterop.SetEditorSelection(yaml));
        }

        public static string BuildEditorSelectionYaml(IEnumerable<InstanceId?> selection)
        {
            var serializer = new SerializerBuilder().Build();
            return serializer.Serialize(selection?
                .Where(id => id is not null && !id.IsEmpty())
                .Select(id => id!.Value)
                .ToArray() ?? Array.Empty<string>());
        }

        public bool ExportPathTracedImage(string outputPath, InstanceId targetInstance = null, uint height = 720, uint samplesPerPixel = 64, uint maxBounces = 4)
        {
            string strInstanceId = targetInstance?.Value ?? string.Empty;
            return InvokeRunningInterop(() => EngineAppInterop.RenderPathTracedImage(outputPath, strInstanceId, height, samplesPerPixel, maxBounces));
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
    }
}
