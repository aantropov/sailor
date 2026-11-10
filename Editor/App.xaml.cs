#if MACCATALYST
using SailorEditor.Platforms.MacCatalyst;
#endif
using SailorEditor.Mcp;

[assembly: XamlCompilation(XamlCompilationOptions.Compile)]

namespace SailorEditor
{
    public partial class App : Application
    {
        public App()
        {
            InitializeComponent();
            MainPage = new AppShell();
        }
        protected override Window CreateWindow(IActivationState state)
        {
            var window = base.CreateWindow(state);
            window.MinimumWidth = 1024;
            window.MinimumHeight = 768;
            window.Title = "Engine Mode";
#if LINUX
            window.Width = 1440;
            window.Height = 900;
#endif
            window.Destroying += async (_, _) =>
                await MauiProgram.GetService<McpEditorHostService>().StopAsync();
#if MACCATALYST
            MacCatalystWindowChrome.SetTitle(window.Title);
#endif

            return window;
        }
    }
}
