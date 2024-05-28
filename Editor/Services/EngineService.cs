using Microsoft.UI.Xaml.Controls.Primitives;
using SailorEditor.ViewModels;
using System.Runtime.InteropServices;
using WinRT;
using YamlDotNet.RepresentationModel;
using YamlDotNet.Serialization.NodeTypeResolvers;

namespace SailorEditor.Engine
{
    public class AppInterop
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
    }

    public class PropertyBase
    {
        public string Typename { get; set; }
    }

    public class Property<T> : PropertyBase
    {
        public Property() { Typename = typeof(T).Name; }
        public T DefaultValue { get; set; } = default;
    };

    public class FloatProperty : Property<float> { }
    public class Vec4Property : Property<Vec4> { }
    public class AssetUIDProperty : Property<AssetUID> { }

    public class ComponentType
    {
        public string Name { get; set; }
        public Dictionary<string, PropertyBase> Properties { get; set; }
    };
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

        public Dictionary<string, Engine.ComponentType> ComponentTypes { get; private set; } = new Dictionary<string, Engine.ComponentType>();

        public void RunProcess(bool bDebug, string commandlineArgs)
        {
            ReadEngineTypes();

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
                    Engine.AppInterop.Initialize(args, args.Length);

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

                    Engine.AppInterop.Start();
                    Engine.AppInterop.Stop();

                    cts.Cancel();

                    Engine.AppInterop.Shutdown();
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

            uint actualNumMessages = Engine.AppInterop.GetMessages(messagesPtrs, numMessages);

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

        void ReadEngineTypes()
        {
            var engineTypes = EngineCacheDirectory + "\\EngineTypes.yaml";

            Dictionary<string, object> res = new Dictionary<string, object>();
            using (var yamlAssetInfo = new FileStream(engineTypes, FileMode.Open))
            using (var reader = new StreamReader(yamlAssetInfo))
            {
                var yaml = new YamlStream();
                yaml.Load(reader);

                var root = (YamlMappingNode)yaml.Documents[0].RootNode;

                foreach (var e in (root["engineTypes"] as YamlNode).AllNodes)
                {
                    
                    res[e[0].ToString()] = e[1].ToString();
                }

                yamlAssetInfo.Close();
            }
        }
    }
}
