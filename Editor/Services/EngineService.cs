using SailorEngine;
using System.Runtime.InteropServices;
using System.Diagnostics;
using YamlDotNet.Serialization;
using SailorEditor.Utility;
using System.Threading;

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

        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern void Initialize(string[] commandLineArgs, int num);
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern void Start();
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern void Stop();
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern void Shutdown();
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern uint GetMessages(nint[] messages, uint num);
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern uint SerializeCurrentWorld(nint[] yamlNode);
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern uint SerializeEngineTypes(nint[] yamlNode);
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
        public const ulong SceneViewportId = 1;

        readonly object interopLock = new();
        readonly object runLock = new();
        readonly RingBufferedBatcher<string> consoleMessages = new(MaxBufferedConsoleMessages);
        int consoleDispatchScheduled = 0;
        bool isRunning = false;
        const int MaxBufferedConsoleMessages = 1000;

        readonly string repoRoot = ResolveRepoRoot();

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

        public string EngineTypesCacheFilePath => Path.Combine(repoRoot, "Cache", "EngineTypes.yaml");

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

        public EngineTypes EngineTypes { get; private set; } = new();

        public string[] GetRecentConsoleMessages() => consoleMessages.Snapshot();

        public void PushConsoleMessage(string message)
        {
            if (string.IsNullOrWhiteSpace(message))
            {
                return;
            }

            PublishConsoleMessages([message]);
        }

        void PublishConsoleMessages(string[] messages)
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

            consoleMessages.EnqueueRange(filtered);
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
                var pending = consoleMessages.DrainPending();
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

        public static void ShowMainWindow(bool bShow) => EngineAppInterop.ShowMainWindow(bShow);

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
            if (hostHandle == nint.Zero)
            {
                return;
            }

            lock (interopLock)
            {
                EngineAppInterop.SetRemoteViewportMacHostHandle(viewportId, 2u, (ulong)hostHandle);
            }
#endif
        }

        public bool TryUpdateRemoteViewport(ulong viewportId, Rect rect, bool visible, bool focused)
        {
#if WINDOWS || MACCATALYST
            if (rect.IsEmpty)
            {
                return false;
            }

            lock (interopLock)
            {
                return EngineAppInterop.UpsertRemoteViewport(viewportId, (uint)rect.X, (uint)rect.Y, (uint)rect.Width, (uint)rect.Height, visible, focused);
            }
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

            lock (interopLock)
            {
                EngineAppInterop.SetViewport((uint)rect.X, (uint)rect.Y, (uint)rect.Width, (uint)rect.Height);
            }
        }

        public void SetEditorRenderTargetSize(uint width, uint height)
        {
#if MACCATALYST
            lock (interopLock)
            {
                EngineAppInterop.SetEditorRenderTargetSize(Math.Max(width, 1u), Math.Max(height, 1u));
            }
#endif
        }

        public void DestroyRemoteViewport(ulong viewportId)
        {
#if WINDOWS || MACCATALYST
            lock (interopLock)
            {
                EngineAppInterop.DestroyRemoteViewport(viewportId);
            }
#endif
        }

        public RemoteViewportSessionState GetRemoteViewportState(ulong viewportId)
        {
#if WINDOWS || MACCATALYST
            lock (interopLock)
            {
                return (RemoteViewportSessionState)EngineAppInterop.GetRemoteViewportState(viewportId);
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
                EngineAppInterop.RetryRemoteViewport(viewportId);
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
                return EngineAppInterop.SendRemoteViewportInput(viewportId, (uint)kind, pointerX, pointerY, wheelDeltaX, wheelDeltaY, keyCode, button, (uint)modifiers, pressed, focused, captured);
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

        public void RunProcess(bool bDebug, string commandlineArgs)
        {
#if WINDOWS || MACCATALYST
            lock (runLock)
            {
                if (isRunning)
                    return;

                isRunning = true;
            }

#if WINDOWS
            nint handle = 0;
#endif
            try
            {
#if WINDOWS
                var window = Application.Current?.Windows?.FirstOrDefault();
                if (window?.Handler?.PlatformView is MauiWinUIWindow mauiWindow)
                {
                    handle = mauiWindow.WindowHandle;
                }

                if (handle == 0)
                {
                    lock (runLock)
                    {
                        isRunning = false;
                    }
                    return;
                }
#endif
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Cannot resolve engine host window handle: {ex.Message}");
                lock (runLock)
                {
                    isRunning = false;
                }
                return;
            }

            string workspace = EngineWorkingDirectory.Replace("\\", "/");
#if WINDOWS
            string commandArgs = $"--noconsole --hwnd {handle} --editor " + commandlineArgs;
            var args = new string[] { bDebug ? PathToEngineExecDebug : PathToEngineExec, "--workspace", workspace, "--world Editor.world" }.Concat(commandArgs.Split(" ", StringSplitOptions.RemoveEmptyEntries)).ToArray();
#else
            string commandArgs = $"--noconsole --editor {commandlineArgs}";
            var args = new string[] { "SailorEditor", "--workspace", workspace, "--world", "Editor.world" }.Concat(commandArgs.Split(" ", StringSplitOptions.RemoveEmptyEntries)).ToArray();
#endif

            try
            {
                TryLoadEngineTypesFromFile("startup cache");

                //ProcessStartInfo startInfo = new ProcessStartInfo
                //{
                //    FileName = GetPathToEngineExec(bDebug),
                //    Arguments = commandArgs + $" --workspace ../",
                //    WorkingDirectory = GetEngineWorkingDirectory(),
                //    UseShellExecute = false
                //};
                //Process process = new Process { StartInfo = startInfo };
                //process.Start();

                var cts = new CancellationTokenSource();

                Task.Run(async () =>
                {
                    try
                    {
#if MACCATALYST
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

                    // Required startup order:
                    // 1) engine types, 2) world, 3) initial messages
                    string serializedEngineTypes = SerializeEngineTypes();
                    if (!string.IsNullOrEmpty(serializedEngineTypes))
                    {
                        SaveEngineTypesToFile(serializedEngineTypes);
                        EngineTypes = EngineTypes.FromYaml(serializedEngineTypes);
                    }
                    else
                    {
                        TryLoadEngineTypesFromFile("empty interop export");
                    }

                    string serializedWorld = SerializeWorld();
                    if (!string.IsNullOrEmpty(serializedWorld))
                    {
                        MainThread.BeginInvokeOnMainThread(() => OnUpdateCurrentWorldAction?.Invoke(serializedWorld));
                    }

                    var bootstrapMessages = PullMessages();
                    if (bootstrapMessages != null)
                    {
                        PublishConsoleMessages(bootstrapMessages);
                    }

#if !MACCATALYST
                        StartPeriodicTask(async () =>
                        {
                            if (!TryUpdateRemoteViewport(SceneViewportId, Viewport, visible: true, focused: false))
                            {
                                lock (interopLock)
                                {
                                    EngineAppInterop.SetViewport((uint)Viewport.X, (uint)Viewport.Y, (uint)Viewport.Width, (uint)Viewport.Height);
                                }
                            }
                        }, 500, 100, cts.Token);
#endif

                    StartPeriodicTask(async () =>
                    {
                        var messages = PullMessages();
                        if (messages != null)
                        {
                            PublishConsoleMessages(messages);
                        }
                    }, 300, 500, cts.Token);

                        StartPeriodicTask(async () =>
                        {
                            string serializedWorld = SerializeWorld();
                            if (serializedWorld != string.Empty)
                                MainThread.BeginInvokeOnMainThread(() => OnUpdateCurrentWorldAction?.Invoke(serializedWorld));

                        }, 1500, 0, cts.Token);

                        EngineAppInterop.Start();
                        EngineAppInterop.Stop();

                        cts.Cancel();
                        DestroyRemoteViewport(SceneViewportId);

                        lock (interopLock)
                        {
                            EngineAppInterop.Shutdown();
                        }
                    }
                    catch (Exception ex)
                    {
                        Console.WriteLine($"SailorEngine task failed: {ex.Message}");
                        TryLoadEngineTypesFromFile("engine startup failed");
                    }
                    finally
                    {
                        lock (runLock)
                        {
                            isRunning = false;
                        }
                    }
                });
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Cannot run SailorEngine process: {ex.Message}");
                lock (runLock)
                {
                    isRunning = false;
                }
            }
#endif
        }

        public void StartPeriodicTask(Func<Task> action, int initialDelay, int periodMs, CancellationToken token)
        {
            Task.Run(async () =>
            {
                try
                {
                    await Task.Delay(initialDelay, token);
                }
                catch (TaskCanceledException)
                {
                    return;
                }

                while (!token.IsCancellationRequested)
                {
                    await action();

                    if (periodMs == 0)
                    {
                        break;
                    }

                    try
                    {
                        await Task.Delay(periodMs, token);
                    }
                    catch (TaskCanceledException)
                    {
                        break;
                    }
                }
            }, token);
        }

        string[] PullMessages()
        {
            uint numMessages = 64;
            nint[] messagesPtrs = new nint[numMessages];

            uint actualNumMessages;
            lock (interopLock)
            {
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

        string SerializeWorld()
        {
            nint[] yamlNodeChar = new nint[1];
            uint numChars;
            lock (interopLock)
            {
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

        string SerializeEngineTypes()
        {
            nint[] yamlNodeChar = new nint[1];
            uint numChars;
            lock (interopLock)
            {
                numChars = EngineAppInterop.SerializeEngineTypes(yamlNodeChar);
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

        bool TryLoadEngineTypesFromFile(string reason)
        {
            try
            {
                if (!File.Exists(EngineTypesCacheFilePath))
                {
                    return false;
                }

                string yaml = File.ReadAllText(EngineTypesCacheFilePath);
                if (string.IsNullOrWhiteSpace(yaml))
                {
                    return false;
                }

                var engineTypes = EngineTypes.FromYaml(yaml);
                if (engineTypes.Components.Count == 0 && engineTypes.Enums.Count == 0)
                {
                    return false;
                }

                EngineTypes = engineTypes;
                Console.WriteLine($"Loaded engine types from cache ({reason}): {EngineTypesCacheFilePath}");
                return true;
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Cannot load engine types cache ({reason}): {ex.Message}");
                return false;
            }
        }

        void SaveEngineTypesToFile(string yaml)
        {
            if (string.IsNullOrWhiteSpace(yaml))
            {
                return;
            }

            try
            {
                string directory = Path.GetDirectoryName(EngineTypesCacheFilePath);
                if (!string.IsNullOrEmpty(directory))
                {
                    Directory.CreateDirectory(directory);
                }

                File.WriteAllText(EngineTypesCacheFilePath, yaml);
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Cannot save engine types cache: {ex.Message}");
            }
        }

        public bool CommitChanges(InstanceId id, string yamlChanges)
        {
            var stringId = id.Value.ToString();
            bool result;

            lock (interopLock)
            {
                result = EngineAppInterop.UpdateObject(stringId, yamlChanges);
            }

            if (result)
            {
                RefreshCurrentWorld();
            }

            return result;
        }

        public void RefreshCurrentWorld()
        {
            using var perfScope = EditorPerf.Scope("EngineService.RefreshCurrentWorld");
            string serializedWorld = SerializeWorld();
            if (!string.IsNullOrEmpty(serializedWorld))
            {
                if (MainThread.IsMainThread)
                {
                    OnUpdateCurrentWorldAction?.Invoke(serializedWorld);
                }
                else
                {
                    MainThread.BeginInvokeOnMainThread(() => OnUpdateCurrentWorldAction?.Invoke(serializedWorld));
                }
            }
        }

        public bool ReparentObject(InstanceId instanceId, InstanceId parentId, bool keepWorldTransform = true)
        {
            var stringId = instanceId?.Value ?? string.Empty;
            var stringParentId = parentId?.Value ?? string.Empty;
            bool result;

            lock (interopLock)
            {
                result = EngineAppInterop.ReparentObject(stringId, stringParentId, keepWorldTransform);
            }

            if (result)
            {
                RefreshCurrentWorld();
            }

            return result;
        }

        public bool CreateGameObject(InstanceId parentId = null)
        {
            var stringParentId = parentId?.Value ?? string.Empty;
            bool result;

            lock (interopLock)
            {
                result = EngineAppInterop.CreateGameObject(stringParentId);
            }

            if (result)
            {
                RefreshCurrentWorld();
            }

            return result;
        }

        public bool DestroyObject(InstanceId instanceId)
        {
            var stringId = instanceId?.Value ?? string.Empty;
            bool result;

            lock (interopLock)
            {
                result = EngineAppInterop.DestroyObject(stringId);
            }

            if (result)
            {
                RefreshCurrentWorld();
            }

            return result;
        }

        public bool ResetComponentToDefaults(InstanceId instanceId)
        {
            var stringId = instanceId?.Value ?? string.Empty;
            bool result;

            lock (interopLock)
            {
                result = EngineAppInterop.ResetComponentToDefaults(stringId);
            }

            if (result)
            {
                RefreshCurrentWorld();
            }

            return result;
        }

        public bool AddComponent(InstanceId instanceId, string componentTypeName)
        {
            var stringId = instanceId?.Value ?? string.Empty;
            bool result;

            lock (interopLock)
            {
                result = EngineAppInterop.AddComponent(stringId, componentTypeName ?? string.Empty);
            }

            if (result)
            {
                RefreshCurrentWorld();
            }

            return result;
        }

        public bool RemoveComponent(InstanceId instanceId)
        {
            var stringId = instanceId?.Value ?? string.Empty;
            bool result;

            lock (interopLock)
            {
                result = EngineAppInterop.RemoveComponent(stringId);
            }

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
            bool result;

            lock (interopLock)
            {
                result = EngineAppInterop.InstantiatePrefab(stringFileId, stringParentId);
            }

            if (result)
            {
                RefreshCurrentWorld();
            }

            return result;
        }

        public bool UpdateEditorSelection(IEnumerable<InstanceId?> selection)
        {
            var yaml = BuildEditorSelectionYaml(selection);

            lock (interopLock)
            {
                return EngineAppInterop.SetEditorSelection(yaml);
            }
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
            lock (interopLock)
            {
                return EngineAppInterop.RenderPathTracedImage(outputPath, strInstanceId, height, samplesPerPixel, maxBounces);
            }
        }

        public void RunWorld(string world, bool bDebug)
        {
            try
            {
                string workspace = (EngineWorkingDirectory + Path.DirectorySeparatorChar).Replace("\\", "/");
                string arguments = $"--workspace {workspace} --world {world}";

                ProcessStartInfo startInfo = new ProcessStartInfo
                {
                    FileName = bDebug ? PathToEngineExecDebug : PathToEngineExec,
                    Arguments = arguments,
                    WorkingDirectory = EngineWorkingDirectory,
                    UseShellExecute = false
                };

                Process process = new Process { StartInfo = startInfo };
                process.Start();
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Cannot run SailorEngine process: {ex.Message}");
            }
        }
    }
}
