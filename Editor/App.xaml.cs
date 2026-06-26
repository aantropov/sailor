#if MACCATALYST
using SailorEditor.Platforms.MacCatalyst;
#endif

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
            window.Title = "Sailor Editor";
#if MACCATALYST
            MacCatalystWindowChrome.SetTitle(window.Title);
            MacCatalystWindowChrome.UseCompactTitlebar(window);
#endif

            return window;
        }
    }
}
