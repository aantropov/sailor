using SailorEditor.Services;

namespace SailorEditor.Views
{
    public partial class ToolbarView : ContentView
    {
        readonly EditorToolbarActions actions;

        public ToolbarView()
        {
            InitializeComponent();
            BindingContext = this;
            actions = MauiProgram.GetService<EditorToolbarActions>();
        }

        private async void OnSaveButtonClicked(object sender, EventArgs e) => await actions.SaveAsync();
        private async void OnUndoButtonClicked(object sender, EventArgs e) => await actions.UndoAsync();
        private async void OnRedoButtonClicked(object sender, EventArgs e) => await actions.RedoAsync();
        private void OnPlayButtonClicked(object sender, EventArgs e) => actions.RunWorld(false);
        private void OnPlayDebugButtonClicked(object sender, EventArgs e) => actions.RunWorld(true);
        private async void OnPathTraceSceneButtonClicked(object sender, EventArgs e) => await actions.ExportPathTracedImageAsync(false);
        private async void OnPathTraceSelectionButtonClicked(object sender, EventArgs e) => await actions.ExportPathTracedImageAsync(true);
        private async void OnSaveLayoutButtonClicked(object sender, EventArgs e) => await actions.SaveLayoutAsync();
        private async void OnResetLayoutButtonClicked(object sender, EventArgs e) => await actions.ResetLayoutAsync();
        private async void OnSettingsButtonClicked(object sender, EventArgs e) => await actions.OpenSettingsAsync();
    }
}
