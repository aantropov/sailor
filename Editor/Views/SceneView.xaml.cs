using SailorEditor.Services;

namespace SailorEditor.Views
{
    public partial class SceneView : ContentView
    {
        public SceneView()
        {
            InitializeComponent();
            MauiProgram.GetService<EngineService>().RunProcess(RunDebugConfigurationCheckBox.IsChecked, "");
        }
        private void OnRunSailorEngineClicked(object sender, EventArgs e)
        {
            string commandLineArgs = WaitForDebuggerAttachedCheckBox.IsChecked ? "--waitfordebugger" : "";
            MauiProgram.GetService<EngineService>().RunProcess(RunDebugConfigurationCheckBox.IsChecked, commandLineArgs);
        }
    }
}