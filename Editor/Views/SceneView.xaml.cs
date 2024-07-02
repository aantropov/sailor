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

            Loaded += (sender, args) =>
            {
                isRunning = true;

                Dispatcher.StartTimer(TimeSpan.FromMilliseconds(500), () =>
                {
                    if (isRunning)
                        UpdateEngineViewport();

                    return isRunning;
                });
            };

            Unloaded += (sender, args) => isRunning = false;
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

        bool isRunning = false;
    }
}