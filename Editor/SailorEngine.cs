using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace Editor
{
    internal class SailorEngine
    {
        static string GetEngineWorkingDirectory()
        {
            string currentDirectory = AppContext.BaseDirectory;
            string directoryFiveLevelsUp = Path.GetFullPath(Path.Combine(currentDirectory, "..", "..", "..", "..", ".."));
            return directoryFiveLevelsUp + "\\";
        }
        static string GetPathToEngineExec(bool bIsDebug)
        {
            return GetEngineWorkingDirectory() + (bIsDebug ? "SailorEngine-Debug.exe" : "SailorEngine-Release.exe");
        }

        internal static void RunProcess(bool bDebug, string commandlineArgs)
        {
#if WINDOWS
            IntPtr handle = ((MauiWinUIWindow)App.Current.Windows[0].Handler.PlatformView).WindowHandle;

            string commandArgs = $"--hwnd {handle} --editor " + commandlineArgs;

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
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Cannot run SailorEngine process: {ex.Message}");
            }
#endif
        }
    }
}
