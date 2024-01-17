using System.Diagnostics;
using System.Runtime.InteropServices;

namespace Editor
{
    public partial class SceneView : ContentView
    {
        public SceneView()
        {
            BackgroundColor = Colors.Gray;
            InitializeComponent();
        }

        private void OnRunSailorEngineClicked(object sender, EventArgs e)
        {
            string commandLineArgs = WaitForDebuggerAttachedCheckBox.IsChecked ? "--waitfordebugger" : "";
            SailorEngine.RunProcess(RunDebugConfigurationCheckBox.IsChecked, commandLineArgs);
        }
    }
}