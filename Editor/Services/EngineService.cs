using SailorEngine;
using System.Runtime.InteropServices;
using System.Diagnostics;

namespace SailorEngine
{
    public class EngineAppInterop
    {
#if DEBUG
        [DllImport("../../../../../Sailor-RelWithDebInfo.dll", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)]
        public static extern void Initialize(string[] commandLineArgs, int num);

        [DllImport("../../../../../Sailor-RelWithDebInfo.dll", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)]
        public static extern void Start();

        [DllImport("../../../../../Sailor-RelWithDebInfo.dll", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)]
        public static extern void Stop();

        [DllImport("../../../../../Sailor-RelWithDebInfo.dll", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)]
        public static extern void Shutdown();

        [DllImport("../../../../../Sailor-RelWithDebInfo.dll", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)]
        public static extern uint GetMessages(nint[] messages, uint num);

        [DllImport("../../../../../Sailor-RelWithDebInfo.dll", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)]
        public static extern uint SerializeCurrentWorld(nint[] yamlNode);

        [DllImport("../../../../../Sailor-RelWithDebInfo.dll", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)]
        public static extern uint SerializeEngineTypes(nint[] yamlNode);

        [DllImport("../../../../../Sailor-RelWithDebInfo.dll", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)]
        public static extern void SetViewport(uint windowPosX, uint windowPosY, uint width, uint height);

        [DllImport("../../../../../Sailor-RelWithDebInfo.dll", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)]
        public static extern bool UpdateObject(string strInstanceId, string strYamlNode);

        [DllImport("../../../../../Sailor-RelWithDebInfo.dll", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)]
        public static extern void ShowMainWindow(bool bShow);

        [DllImport("../../../../../Sailor-RelWithDebInfo.dll", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)]
        public static extern bool RenderPathTracedImage(string strOutputPath, string strInstanceId, uint height, uint samplesPerPixel, uint maxBounces);

#else
        [DllImport("../../../../../Sailor-Release.dll", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)]
        public static extern void Initialize(string[] commandLineArgs, int num);

        [DllImport("../../../../../Sailor-Release.dll", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)]
        public static extern void Start();

        [DllImport("../../../../../Sailor-Release.dll", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)]
        public static extern void Stop();

        [DllImport("../../../../../Sailor-Release.dll", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)]
        public static extern void Shutdown();

        [DllImport("../../../../../Sailor-Release.dll", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)]
        public static extern uint GetMessages(nint[] messages, uint num);

        [DllImport("../../../../../Sailor-Release.dll", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)]
        public static extern uint SerializeCurrentWorld(nint[] yamlNode);

        [DllImport("../../../../../Sailor-Release.dll", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)]
        public static extern uint SerializeEngineTypes(nint[] yamlNode);

        [DllImport("../../../../../Sailor-Release.dll", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)]
        public static extern void SetViewport(uint windowPosX, uint windowPosY, uint width, uint height);

        [DllImport("../../../../../Sailor-Release.dll", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)]
        public static extern bool UpdateObject(string strInstanceId, string strYamlNode);

        [DllImport("../../../../../Sailor-Release.dll", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)]
        public static extern void ShowMainWindow(bool bShow);

        [DllImport("../../../../../Sailor-Release.dll", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)]
        public static extern bool RenderPathTracedImage(string strOutputPath, string strInstanceId, uint height, uint samplesPerPixel, uint maxBounces);
#endif
    }
}

namespace SailorEditor.Services
{
    internal class EngineService
    {
        readonly object interopLock = new();
        readonly object runLock = new();
        bool isRunning = false;

        public string EngineContentDirectory { get { return Path.Combine(EngineWorkingDirectory, "..", "Content"); } }

        public string EngineCacheDirectory { get { return Path.Combine(EngineWorkingDirectory, "..", "Cache"); } }

        public string EngineWorkingDirectory { get { return Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "..")) + "\\"; } }

        public string PathToEngineExecDebug { get { return EngineWorkingDirectory + "SailorEngine-Debug.exe"; } }

        public string PathToEngineExec { get { return EngineWorkingDirectory + "SailorEngine-Release.exe"; } }

        public Rect Viewport { get; set; } = new Rect(0, 0, 1024, 768);

        public event Action<string[]> OnPullMessagesAction = delegate { };
        public event Action<string> OnUpdateCurrentWorldAction = delegate { };

        public EngineTypes EngineTypes { get; private set; } = new();

        public static void ShowMainWindow(bool bShow) => EngineAppInterop.ShowMainWindow(bShow);

        public void RunProcess(bool bDebug, string commandlineArgs)
        {
#if WINDOWS
            lock (runLock)
            {
                if (isRunning)
                    return;

                isRunning = true;
            }

            nint handle = 0;
            try
            {
                var window = Application.Current?.Windows?.FirstOrDefault();
                if (window?.Handler?.PlatformView is MauiWinUIWindow mauiWindow)
                {
                    handle = mauiWindow.WindowHandle;
                }
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

            if (handle == 0)
            {
                lock (runLock)
                {
                    isRunning = false;
                }
                return;
            }

            string commandArgs = $"--noconsole --hwnd {handle} --editor " + commandlineArgs;
            string workspace = (Path.GetFullPath(Path.Combine(EngineWorkingDirectory, "..")) + "\\").Replace("\\", "/");
            var args = new string[] { bDebug ? PathToEngineExecDebug : PathToEngineExec, "--workspace", workspace, "--world Editor.world" }.Concat(commandArgs.Split(" ")).ToArray();

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
                        lock (interopLock)
                        {
                            EngineAppInterop.Initialize(args, args.Length);
                        }

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
                        lock (interopLock)
                        {
                            EngineAppInterop.SetViewport((uint)Viewport.X, (uint)Viewport.Y, (uint)Viewport.Width, (uint)Viewport.Height);
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
