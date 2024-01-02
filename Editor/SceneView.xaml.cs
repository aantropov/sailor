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
            SailorEngine.RunProcess(RunDebugConfigurationCheckBox.IsChecked, WaitForDebuggerAttachedCheckBox.IsChecked ? "--waitfordebugger" : "");
        }
    }
}