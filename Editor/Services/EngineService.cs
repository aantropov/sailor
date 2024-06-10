using SailorEngine;
using SailorEditor.ViewModels;
using System.Reflection.Metadata;
using System.Runtime.InteropServices;
using WinRT;
using YamlDotNet.RepresentationModel;
using YamlDotNet.Serialization.NodeTypeResolvers;

namespace SailorEngine
{
    public class EngineAppInterop
    {
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

        public event Action<string[]> OnPullMessagesAction = delegate { };

        public Dictionary<string, ComponentType> ComponentTypes { get; private set; } = new Dictionary<string, ComponentType>();

        public void RunProcess(bool bDebug, string commandlineArgs)
        {
            var engineTypesYaml = EngineCacheDirectory + "\\EngineTypes.yaml";
            ComponentTypes = Helper.ReadEngineTypes(engineTypesYaml);

#if WINDOWS

            nint handle = ((MauiWinUIWindow)Application.Current.Windows[0].Handler.PlatformView).WindowHandle;

            string commandArgs = $"--noconsole --hwnd {handle} --editor " + commandlineArgs;
            string workspace = (Path.GetFullPath(Path.Combine(EngineWorkingDirectory, "..")) + "\\").Replace("\\", "/");
            var args = new string[] { bDebug ? PathToEngineExecDebug : PathToEngineExec, "--workspace", workspace }.Concat(commandArgs.Split(" ")).ToArray();

            try
            {
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

                    Task.Run(async () =>
                    {
                        while (!cts.Token.IsCancellationRequested)
                        {
                            PullMessages();
                            try
                            {
                                await Task.Delay(500);
                            }
                            catch (TaskCanceledException)
                            {
                                break;
                            }
                        }
                    });

                    Task.Run(async () =>
                    {
                        string serializedWorld = string.Empty;
                        while (!cts.Token.IsCancellationRequested && serializedWorld == string.Empty)
                        {
                            try
                            {
                                await Task.Delay(5000);
                            }
                            catch (TaskCanceledException)
                            {
                                break;
                            }

                            serializedWorld = SerializeWorld();
                        }
                    });

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

        void PullMessages()
        {
            uint numMessages = 64;
            nint[] messagesPtrs = new nint[numMessages];

            uint actualNumMessages = EngineAppInterop.GetMessages(messagesPtrs, numMessages);

            if (actualNumMessages == 0)
                return;

            string[] messages = new string[actualNumMessages];
            for (int i = 0; i < actualNumMessages; i++)
            {
                messages[i] = Marshal.PtrToStringAnsi(messagesPtrs[i]);
                Marshal.FreeHGlobal(messagesPtrs[i]);
            }

            MainThread.BeginInvokeOnMainThread(() => OnPullMessagesAction?.Invoke(messages));
        }

        string SerializeWorld()
        {
            nint[] yamlNodeChar = new nint[1];

            uint numChars = EngineAppInterop.SerializeCurrentWorld(yamlNodeChar);

            if (numChars == 0)
                return string.Empty;

            string yamlNode = Marshal.PtrToStringAnsi(yamlNodeChar[0]);
            Marshal.FreeHGlobal(yamlNodeChar[0]);

            return yamlNode;
        }
    }
}
