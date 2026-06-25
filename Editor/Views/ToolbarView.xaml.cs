using SailorEditor.Services;

namespace SailorEditor.Views
{
    public partial class ToolbarView : ContentView
    {
        readonly EditorToolbarActions actions;
        readonly WorkspaceUiService workspaceUi;

        public ToolbarView()
        {
            InitializeComponent();
            BindingContext = this;
            actions = MauiProgram.GetService<EditorToolbarActions>();
            workspaceUi = MauiProgram.GetService<WorkspaceUiService>();
            workspaceUi.ProjectionChanged += OnWorkspaceProjectionChanged;
            UpdateWorkspaceProjection();
        }

        private async void OnNewWorkspaceButtonClicked(object sender, EventArgs e) => await workspaceUi.NewWorkspaceAsync();
        private async void OnOpenWorkspaceButtonClicked(object sender, EventArgs e) => await workspaceUi.OpenWorkspaceAsync();
        private async void OnSaveWorkspaceButtonClicked(object sender, EventArgs e) => await workspaceUi.SaveWorkspaceAsync();
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

        void OnWorkspaceProjectionChanged(object? sender, EventArgs e)
        {
            MainThread.BeginInvokeOnMainThread(UpdateWorkspaceProjection);
        }

        void UpdateWorkspaceProjection()
        {
            WorkspaceNameLabel.Text = workspaceUi.Projection.ToolbarText;
        }
    }
}
