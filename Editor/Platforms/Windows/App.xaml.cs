using Microsoft.UI.Xaml;
using SailorEditor.Testing;

// To learn more about WinUI, the WinUI project structure,
// and more about our project templates, see: http://aka.ms/winui-project-info.

namespace SailorEditor.WinUI
{
    /// <summary>
    /// Provides application-specific behavior to supplement the default Application class.
    /// </summary>
    public partial class App : MauiWinUIApplication
    {
        /// <summary>
        /// Initializes the singleton application object.  This is the first line of authored code
        /// executed, and as such is the logical equivalent of main() or WinMain().
        /// </summary>
        public App()
        {
            if (EditorScreenshotTestDiagnostics.IsEnabled)
            {
                UnhandledException += OnWinUiUnhandledException;
                AppDomain.CurrentDomain.UnhandledException += OnAppDomainUnhandledException;
                TaskScheduler.UnobservedTaskException += OnUnobservedTaskException;
                EditorScreenshotTestDiagnostics.TryWriteCheckpoint("WinUI App constructor");
            }

            try
            {
                InitializeComponent();
                EditorScreenshotTestDiagnostics.TryWriteCheckpoint("WinUI App.InitializeComponent completed");
            }
            catch (Exception ex)
            {
                EditorScreenshotTestDiagnostics.TryWriteUnhandledException("WinUI App.InitializeComponent", ex);
                throw;
            }
        }

        protected override MauiApp CreateMauiApp()
        {
            try
            {
                EditorScreenshotTestDiagnostics.TryWriteCheckpoint("MauiProgram.CreateMauiApp starting");
                var app = MauiProgram.CreateMauiApp();
                EditorScreenshotTestDiagnostics.TryWriteCheckpoint("MauiProgram.CreateMauiApp completed");
                return app;
            }
            catch (Exception ex)
            {
                EditorScreenshotTestDiagnostics.TryWriteUnhandledException("MauiProgram.CreateMauiApp", ex);
                throw;
            }
        }

        static void OnWinUiUnhandledException(object sender, Microsoft.UI.Xaml.UnhandledExceptionEventArgs args)
            => EditorScreenshotTestDiagnostics.TryWriteUnhandledException("WinUI Application.UnhandledException", args.Exception);

        static void OnAppDomainUnhandledException(object sender, System.UnhandledExceptionEventArgs args)
            => EditorScreenshotTestDiagnostics.TryWriteUnhandledException("AppDomain.UnhandledException", args.ExceptionObject);

        static void OnUnobservedTaskException(object? sender, UnobservedTaskExceptionEventArgs args)
            => EditorScreenshotTestDiagnostics.TryWriteUnhandledException("TaskScheduler.UnobservedTaskException", args.Exception);
    }
}
