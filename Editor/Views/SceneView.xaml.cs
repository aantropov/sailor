using System.Diagnostics;
using System.Runtime.InteropServices;

namespace Editor.Views
{
    public partial class SceneView : ContentView
    {
        public SceneView()
        {
            InitializeComponent();
        }

        private void OnRunSailorEngineClicked(object sender, EventArgs e)
        {
            string commandLineArgs = WaitForDebuggerAttachedCheckBox.IsChecked ? "--waitfordebugger" : "";
            SailorEngine.RunProcess(RunDebugConfigurationCheckBox.IsChecked, commandLineArgs);
        }
    }
}