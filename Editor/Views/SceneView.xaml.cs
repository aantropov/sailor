using SailorEditor.Helpers;
using SailorEditor.Services;

namespace SailorEditor.Views
{
    public partial class SceneView : ContentView
    {
        public SceneView()
        {
            InitializeComponent();
            MauiProgram.GetService<EngineService>().RunProcess(RunDebugConfigurationCheckBox.IsChecked, "");

            SizeChanged += (sender, args) => UpdateEngineViewport();
            Loaded += (sender, args) => UpdateEngineViewport();

#if WINDOWS
            var window = Microsoft.Maui.Controls.Application.Current.Windows[0].Handler.PlatformView as Microsoft.UI.Xaml.Window;
            if (window != null)
            {
                window.SizeChanged += (sender, args) => UpdateEngineViewport();
                window.Activated += (object sender, Microsoft.UI.Xaml.WindowActivatedEventArgs e) =>
                {
                    if (e.WindowActivationState == Microsoft.UI.Xaml.WindowActivationState.CodeActivated ||
                        e.WindowActivationState == Microsoft.UI.Xaml.WindowActivationState.PointerActivated)
                        UpdateEngineViewport();
                };
#endif
            }
        }
        void UpdateEngineViewport()
        {
            var engineService = MauiProgram.GetService<EngineService>();
            var rect = Viewport.GetAbsolutePositionWin();

            if (!rect.IsEmpty)
            {
                engineService.Viewport = rect;
            }
        }

        private void OnRunSailorEngineClicked(object sender, EventArgs e)
        {
            string commandLineArgs = WaitForDebuggerAttachedCheckBox.IsChecked ? "--waitfordebugger" : "";
            MauiProgram.GetService<EngineService>().RunProcess(RunDebugConfigurationCheckBox.IsChecked, commandLineArgs);
        }
    }
}