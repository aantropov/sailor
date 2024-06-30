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

            SizeChanged += OnSizeChanged;
        }

        private void OnSizeChanged(object sender, EventArgs e)
        {
            var engineService = MauiProgram.GetService<EngineService>();
            var rect = this.GetAbsolutePositionWin();

            if (!rect.IsEmpty)
            {
                engineService.SetViewport((uint)rect.X, (uint)rect.Y, (uint)rect.Width, (uint)rect.Height);
            }
        }

        private void OnRunSailorEngineClicked(object sender, EventArgs e)
        {
            string commandLineArgs = WaitForDebuggerAttachedCheckBox.IsChecked ? "--waitfordebugger" : "";
            MauiProgram.GetService<EngineService>().RunProcess(RunDebugConfigurationCheckBox.IsChecked, commandLineArgs);
        }
    }
}