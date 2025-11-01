using SailorEngine;
using SailorEditor.ViewModels;
using System.Reflection.Metadata;
using System.Runtime.InteropServices;
using WinRT;
using YamlDotNet.RepresentationModel;
using YamlDotNet.Serialization.NodeTypeResolvers;
using YamlDotNet.Core.Tokens;
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
#endif
    }
}

namespace SailorEditor.Services
{
    internal class EngineService
    {
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
            nint handle = ((MauiWinUIWindow)Application.Current.Windows[0].Handler.PlatformView).WindowHandle;

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

                Task.Run(() =>
                {
                    EngineAppInterop.Initialize(args, args.Length);

                    StartPeriodicTask(() =>
                    {
                        EngineAppInterop.SetViewport((uint)Viewport.X, (uint)Viewport.Y, (uint)Viewport.Width, (uint)Viewport.Height);
                        return Task.CompletedTask;
                    }, 500, 100, cts.Token);

                    StartPeriodicTask(() =>
                    {
                        var messages = PullMessages();
                        if (messages != null)
                        {
                            MainThread.BeginInvokeOnMainThread(() => OnPullMessagesAction?.Invoke(messages));
                        }
                        return Task.CompletedTask;
                    }, 300, 500, cts.Token);

                    StartPeriodicTask(() =>
                    {
                        string serializedEngineTypes = SerializeEngineTypes();
                        EngineTypes = EngineTypes.FromYaml(serializedEngineTypes);
                        return Task.CompletedTask;
                    }, 1000, 0, cts.Token);

                    StartPeriodicTask(() =>
                    {
                        string serializedWorld = SerializeWorld();
                        if (serializedWorld != string.Empty)
                            MainThread.BeginInvokeOnMainThread(() => OnUpdateCurrentWorldAction?.Invoke(serializedWorld));

                        return Task.CompletedTask;
                    }, 1500, 0, cts.Token);

                    EngineAppInterop.Start();
                    EngineAppInterop.Stop();

                    cts.Cancel();

                    EngineAppInterop.Shutdown();
                });
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Cannot run SailorEngine process: {ex.Message}");
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

            uint actualNumMessages = EngineAppInterop.GetMessages(messagesPtrs, numMessages);

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

        static string SerializeWorld()
        {
            nint[] yamlNodeChar = new nint[1];
            uint numChars = EngineAppInterop.SerializeCurrentWorld(yamlNodeChar);

            if (numChars == 0)
                return string.Empty;

            string yamlNode = Marshal.PtrToStringAnsi(yamlNodeChar[0]);
            Marshal.FreeHGlobal(yamlNodeChar[0]);

            return yamlNode;
        }

        static string SerializeEngineTypes()
        {
            nint[] yamlNodeChar = new nint[1];
            uint numChars = EngineAppInterop.SerializeEngineTypes(yamlNodeChar);

            if (numChars == 0)
                return string.Empty;

            string yamlNode = Marshal.PtrToStringAnsi(yamlNodeChar[0]);
            Marshal.FreeHGlobal(yamlNodeChar[0]);

            return yamlNode;
        }

        public bool CommitChanges(InstanceId id, string yamlChanges)
        {
            var stringId = id.Value.ToString();
            return EngineAppInterop.UpdateObject(stringId, yamlChanges);
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
