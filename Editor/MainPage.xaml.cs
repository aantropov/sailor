using System.Diagnostics;
using System.Runtime.InteropServices;

namespace Editor
{
    public partial class MainPage : ContentPage
    {
        static string GetEngineWorkingDirectory()
        {
            string currentDirectory = AppContext.BaseDirectory;
            string directoryFiveLevelsUp = Path.GetFullPath(Path.Combine(currentDirectory, "..", "..", "..", "..", ".."));

            return directoryFiveLevelsUp + "\\";
        }
        static string GetEngineExec(bool bIsDebug)
        {
            return GetEngineWorkingDirectory() + (bIsDebug ? "SailorEngine-Debug.exe" :"SailorEngine-Release.exe");
        }

        public MainPage()
        {
            InitializeComponent();
        }

        private void OnRunSailorEngineClicked(object sender, EventArgs e)
        {
#if WINDOWS

            IntPtr handle = ((MauiWinUIWindow)App.Current.Windows[0].Handler.PlatformView).WindowHandle;
            
            string commandArgs = $"--hwnd {handle} --editor";
            if (WaitForDebuggerAttachedCheckBox.IsChecked)
            {
                commandArgs += " --waitfordebugger";
            }

            ProcessStartInfo startInfo = new ProcessStartInfo
            {
                FileName = GetEngineExec(RunDebugConfigurationCheckBox.IsChecked),
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
                // Обработка исключения
                Console.WriteLine($"Ошибка при запуске процесса: {ex.Message}");
            }
#endif
        }
    }
}