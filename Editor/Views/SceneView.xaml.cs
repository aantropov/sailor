using SailorEditor.Services;

namespace SailorEditor.Views
{
    public partial class SceneView : ContentView
    {
        public SceneView()
        {
            InitializeComponent();
            EngineService.RunProcess(RunDebugConfigurationCheckBox.IsChecked, "");
        }
        private void OnRunSailorEngineClicked(object sender, EventArgs e)
        {
            string commandLineArgs = WaitForDebuggerAttachedCheckBox.IsChecked ? "--waitfordebugger" : "";
            EngineService.RunProcess(RunDebugConfigurationCheckBox.IsChecked, commandLineArgs);
        }
    }
}