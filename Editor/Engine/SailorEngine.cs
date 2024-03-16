using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

public class AppInterop
{
    [DllImport("../../../../../Sailor-Debug.dll", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Initialize(string[] commandLineArgs, int num);

    [DllImport("../../../../../Sailor-Debug.dll", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Start();

    [DllImport("../../../../../Sailor-Debug.dll", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Stop();

    [DllImport("../../../../../Sailor-Debug.dll", CharSet = CharSet.Ansi, CallingConvention = CallingConvention.Cdecl)]
    public static extern void Shutdown();
}

namespace SailorEditor
{
    internal class SailorEngine
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

        internal static void RunProcess(bool bDebug, string commandlineArgs)
        {
#if WINDOWS
            IntPtr handle = ((MauiWinUIWindow)App.Current.Windows[0].Handler.PlatformView).WindowHandle;

            string commandArgs = $"--hwnd {handle} --editor --workspace ../ " + commandlineArgs;

            ProcessStartInfo startInfo = new ProcessStartInfo
            {
                FileName = GetPathToEngineExec(bDebug),
                Arguments = commandArgs,
                WorkingDirectory = GetEngineWorkingDirectory(),
                UseShellExecute = false
            };

            try
            {
                Process process = new Process
                {
                    StartInfo = startInfo
                };
                process.Start();

                /*
                string workspace = (Path.GetFullPath(Path.Combine(GetEngineWorkingDirectory(), "..")) + "\\").Replace("\\", "/");
                var args = new string[] { GetPathToEngineExec(bDebug), "--workspace", workspace }.Concat(commandArgs.Split(" ")).ToArray();

                Task.Run(() =>
                {
                    AppInterop.Initialize(args, args.Length);
                    AppInterop.Start();
                    AppInterop.Stop();
                    AppInterop.Shutdown();
                });
                */
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Cannot run SailorEngine process: {ex.Message}");
            }
#endif
        }
    }
}
