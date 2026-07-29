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

        private void OnSaveButtonClicked(object sender, EventArgs e)
            => RunToolbarAction(actions.SaveAsync, "Save");
        private void OnUndoButtonClicked(object sender, EventArgs e)
            => RunToolbarAction(actions.UndoAsync, "Undo");
        private void OnRedoButtonClicked(object sender, EventArgs e)
            => RunToolbarAction(actions.RedoAsync, "Redo");
        private void OnPlayButtonClicked(object sender, EventArgs e)
            => RunToolbarAction(
                () => actions.RunWorldAsync(false),
                "Play");
        private void OnPlayDebugButtonClicked(object sender, EventArgs e)
            => RunToolbarAction(
                () => actions.RunWorldAsync(true),
                "Debug play");
        private void OnPathTraceSceneButtonClicked(object sender, EventArgs e)
            => RunToolbarAction(
                () => actions.ExportPathTracedImageAsync(false),
                "Path trace scene");
        private void OnPathTraceSelectionButtonClicked(object sender, EventArgs e)
            => RunToolbarAction(
                () => actions.ExportPathTracedImageAsync(true),
                "Path trace selection");
        private void OnSaveLayoutButtonClicked(object sender, EventArgs e)
            => RunToolbarAction(actions.SaveLayoutAsync, "Save layout");
        private void OnResetLayoutButtonClicked(object sender, EventArgs e)
            => RunToolbarAction(actions.ResetLayoutAsync, "Reset layout");
        private void OnSettingsButtonClicked(object sender, EventArgs e)
            => RunToolbarAction(actions.OpenSettingsAsync, "Open settings");

        void RunToolbarAction(
            Func<Task> action,
            string operation)
        {
            _ = RunToolbarActionSafelyAsync(
                action,
                operation);
        }

        async Task RunToolbarActionSafelyAsync(
            Func<Task> action,
            string operation)
        {
            try
            {
                await action();
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"{operation} failed: {ex}");
                try
                {
                    MauiProgram.GetService<SailorEditor.Shell.EditorShellHost>()
                        .SetStatus($"{operation} failed: {ex.Message}");
                }
                catch (Exception statusException)
                {
                    Console.Error.WriteLine(
                        $"Failed to publish toolbar action error status: {statusException}");
                }
            }
        }
    }
}
