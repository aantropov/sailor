using SailorEngine;
using System.Runtime.InteropServices;
using System.Diagnostics;

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
        [return: MarshalAs(UnmanagedType.I1)]
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern bool UpsertRemoteViewport(ulong viewportId, uint windowPosX, uint windowPosY, uint width, uint height, [MarshalAs(UnmanagedType.I1)] bool visible, [MarshalAs(UnmanagedType.I1)] bool focused);
        [return: MarshalAs(UnmanagedType.I1)]
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern bool DestroyRemoteViewport(ulong viewportId);
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern uint GetRemoteViewportState(ulong viewportId);
        [return: MarshalAs(UnmanagedType.I1)]
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern bool RetryRemoteViewport(ulong viewportId);
        [return: MarshalAs(UnmanagedType.I1)]
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern bool SetRemoteViewportMacHostHandle(ulong viewportId, uint hostHandleKind, ulong hostHandleValue);
        [return: MarshalAs(UnmanagedType.I1)]
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern bool UpdateObject(string strInstanceId, string strYamlNode);
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern void ShowMainWindow([MarshalAs(UnmanagedType.I1)] bool bShow);
        [return: MarshalAs(UnmanagedType.I1)]
        [DllImport(EngineLibrary, CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)] public static extern bool RenderPathTracedImage(string strOutputPath, string strInstanceId, uint height, uint samplesPerPixel, uint maxBounces);
    }
}

namespace SailorEditor.Services
{
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
        bool isRunning = false;

        readonly string repoRoot = ResolveRepoRoot();

        public string EngineContentDirectory { get { return Path.Combine(repoRoot, "Content"); } }

        public string EngineCacheDirectory { get { return Path.Combine(repoRoot, "Cache"); } }

        public string EngineWorkingDirectory { get { return repoRoot + Path.DirectorySeparatorChar; } }

        public string PathToEngineExecDebug { get { return Path.Combine(EngineWorkingDirectory, "SailorEngine-Debug.exe"); } }

        public string PathToEngineExec { get { return Path.Combine(EngineWorkingDirectory, "SailorEngine-Release.exe"); } }

        public Rect Viewport { get; set; } = new Rect(0, 0, 1024, 768);

        public event Action<string[]> OnPullMessagesAction = delegate { };
        public event Action<string> OnUpdateCurrentWorldAction = delegate { };

        public EngineTypes EngineTypes { get; private set; } = new();

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

        public void RunProcess(bool bDebug, string commandlineArgs)
        {
#if WINDOWS || MACCATALYST
            lock (runLock)
            {
                if (isRunning)
                    return;

                isRunning = true;
            }

            nint handle = 0;
            try
            {
#if WINDOWS
                var window = Application.Current?.Windows?.FirstOrDefault();
                if (window?.Handler?.PlatformView is MauiWinUIWindow mauiWindow)
                {
                    handle = mauiWindow.WindowHandle;
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

#if WINDOWS
            if (handle == 0)
            {
                lock (runLock)
                {
                    isRunning = false;
                }
                return;
            }
#endif

            string workspace = (Path.GetFullPath(Path.Combine(EngineWorkingDirectory, "..")) + Path.DirectorySeparatorChar).Replace("\\", "/");
#if WINDOWS
            string commandArgs = $"--noconsole --hwnd {handle} --editor " + commandlineArgs;
            var args = new string[] { bDebug ? PathToEngineExecDebug : PathToEngineExec, "--workspace", workspace, "--world Editor.world" }.Concat(commandArgs.Split(" ", StringSplitOptions.RemoveEmptyEntries)).ToArray();
#else
            string commandArgs = $"--noconsole --editor {commandlineArgs}";
            var args = new string[] { "SailorEditor", "--workspace", workspace, "--world", "Editor.world" }.Concat(commandArgs.Split(" ", StringSplitOptions.RemoveEmptyEntries)).ToArray();
#endif

            try
            {
                //var engineTypesYaml = EngineCacheDirectory + "\\EngineTypes.yaml";
                //EngineTypes = EngineTypes.FromFile(engineTypesYaml);

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
                        EngineTypes = EngineTypes.FromYaml(serializedEngineTypes);
                    }

                    string serializedWorld = SerializeWorld();
                    if (!string.IsNullOrEmpty(serializedWorld))
                    {
                        MainThread.BeginInvokeOnMainThread(() => OnUpdateCurrentWorldAction?.Invoke(serializedWorld));
                    }

                    var bootstrapMessages = PullMessages();
                    if (bootstrapMessages != null)
                    {
                        MainThread.BeginInvokeOnMainThread(() => OnPullMessagesAction?.Invoke(bootstrapMessages));
                    }

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

                    StartPeriodicTask(async () =>
                    {
                        var messages = PullMessages();
                        if (messages != null)
                        {
                            MainThread.BeginInvokeOnMainThread(() => OnPullMessagesAction?.Invoke(messages));
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
                Marshal.FreeHGlobal(messagesPtrs[i]);
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

            string yamlNode = Marshal.PtrToStringAnsi(yamlNodeChar[0]);
            Marshal.FreeHGlobal(yamlNodeChar[0]);

            return yamlNode;
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

            string yamlNode = Marshal.PtrToStringAnsi(yamlNodeChar[0]);
            Marshal.FreeHGlobal(yamlNodeChar[0]);

            return yamlNode;
        }

        public bool CommitChanges(InstanceId id, string yamlChanges)
        {
            var stringId = id.Value.ToString();
            lock (interopLock)
            {
                return EngineAppInterop.UpdateObject(stringId, yamlChanges);
            }
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
                string workspace = (Path.GetFullPath(Path.Combine(EngineWorkingDirectory, "..")) + "\\").Replace("\\", "/");
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
