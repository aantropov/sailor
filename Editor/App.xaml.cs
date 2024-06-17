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

            return window;
        }
    }
}