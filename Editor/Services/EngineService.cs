using System.Runtime.InteropServices;

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

namespace SailorEditor.Services
{
    internal class EngineService
    {
        public static string GetEngineContentDirectory()
        {
            return Path.Combine(GetEngineWorkingDirectory(), "..", "Content");
        }
        public static string GetEngineWorkingDirectory()
        {
            string currentDirectory = AppContext.BaseDirectory;
            string directoryFiveLevelsUp = Path.GetFullPath(Path.Combine(currentDirectory, "..", "..", "..", "..", ".."));
            return directoryFiveLevelsUp + "\\";
        }
        public static string GetPathToEngineExec(bool bIsDebug)
        {
            return GetEngineWorkingDirectory() + (bIsDebug ? "SailorEngine-Debug.exe" : "SailorEngine-Release.exe");
        }

        public static event Action<string[]> OnPullMessagesAction = delegate { };

        static void PullMessages()
        {
            uint numMessages = 64;
            nint[] messagesPtrs = new nint[numMessages];

            uint actualNumMessages = AppInterop.GetMessages(messagesPtrs, numMessages);

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

        internal static void RunProcess(bool bDebug, string commandlineArgs)
        {
#if WINDOWS
            nint handle = ((MauiWinUIWindow)Application.Current.Windows[0].Handler.PlatformView).WindowHandle;

            string commandArgs = $"--noconsole --hwnd {handle} --editor " + commandlineArgs;
            string workspace = (Path.GetFullPath(Path.Combine(GetEngineWorkingDirectory(), "..")) + "\\").Replace("\\", "/");
            var args = new string[] { GetPathToEngineExec(bDebug), "--workspace", workspace }.Concat(commandArgs.Split(" ")).ToArray();

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
                    AppInterop.Initialize(args, args.Length);

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

                    AppInterop.Start();
                    AppInterop.Stop();

                    cts.Cancel();

                    AppInterop.Shutdown();
                });

            }
            catch (Exception ex)
            {
                Console.WriteLine($"Cannot run SailorEngine process: {ex.Message}");
            }
#endif
        }
    }
}
