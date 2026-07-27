using SailorEditor.Commands;
using SailorEditor.Panels;
using SailorEditor.Shell;
using SailorEditor.ViewModels;
using SailorEngine;
using ViewModelComponent = SailorEditor.ViewModels.Component;
using ViewModelGameObject = SailorEditor.ViewModels.GameObject;

namespace SailorEditor.Services
{
    public sealed class EditorToolbarActions
    {
        public async Task SaveAsync()
        {
            var selectionService = MauiProgram.GetService<SelectionService>();
            var selected = selectionService.SelectedItem;

            if (selected is IInspectorEditable editable && editable.HasPendingInspectorChanges)
            {
                if (!editable.CommitInspectorChanges())
                {
                    await DisplayStatus("Save Scene", "Inspector changes could not be committed.");
                    return;
                }
            }

            var result = await MauiProgram.GetService<WorldService>().SaveCurrentWorldAsync();
            switch (result.Outcome)
            {
                case SceneSaveOutcome.Saved:
                    await DisplayStatus("Save Scene", $"Saved scene: {result.Path}");
                    break;
                case SceneSaveOutcome.Cancelled:
                    await DisplayStatus("Save Scene", "Scene save cancelled.");
                    break;
                default:
                    await DisplayStatus("Save Scene", result.Error ?? "Scene save failed.");
                    break;
            }
        }

        public async Task NewSceneAsync()
        {
            var page = Application.Current?.Windows.FirstOrDefault()?.Page ?? Application.Current?.MainPage;
            if (page is null)
                return;

            var history = MauiProgram.GetService<ICommandHistoryService>();
            if (history.CanUndo)
            {
                var discard = await page.DisplayAlert(
                    "New Scene",
                    "Create a new scene and discard unsaved editor changes in the current scene?",
                    "Create",
                    "Cancel");
                if (!discard)
                    return;
            }

            var created = await MauiProgram.GetService<WorldService>().CreateNewWorldAsync();
            await DisplayStatus("New Scene", created ? "Created a new untitled scene." : "Unable to create a new scene.");
        }

        public Task UndoAsync()
        {
            return MauiProgram.GetService<ICommandHistoryService>().UndoAsync(new CommandOrigin(CommandOriginKind.UI, "ToolbarUndo"));
        }

        public Task RedoAsync()
        {
            return MauiProgram.GetService<ICommandHistoryService>().RedoAsync(new CommandOrigin(CommandOriginKind.UI, "ToolbarRedo"));
        }

        public void RunWorld(bool debug)
        {
            var worldService = MauiProgram.GetService<WorldService>();
            var engineService = MauiProgram.GetService<EngineService>();

            Task.Run(() =>
            {
                var launchContext = engineService.GetLaunchContext();
                string world = worldService.SerializeCurrentWorld();
                Directory.CreateDirectory(launchContext.CacheDirectory);

                File.WriteAllText(launchContext.TempWorldFilePath, world);

                engineService.RunWorld(launchContext.TempWorldRuntimePath, debug, launchContext);
            });
        }

        public async Task ExportPathTracedImageAsync(bool selectedOnly)
        {
            var engineService = MauiProgram.GetService<EngineService>();
            var selectionService = MauiProgram.GetService<SelectionService>();

            InstanceId targetInstance = null;
            if (selectedOnly)
            {
                var selectedItem = selectionService.SelectedItems.FirstOrDefault();
                switch (selectedItem)
                {
                    case ViewModelComponent component:
                        targetInstance = component.InstanceId;
                        break;

                    case ViewModelGameObject gameObject:
                        targetInstance = gameObject.InstanceId;
                        break;
                }

                if (targetInstance == null || targetInstance.IsEmpty())
                {
                    await DisplayStatus("Path Tracing", "Select a GameObject or a component first.");
                    return;
                }
            }

            var outputDir = Path.Combine(engineService.EngineCacheDirectory, "PathTracing");
            Directory.CreateDirectory(outputDir);

            var mode = selectedOnly ? "selection" : "scene";
            var outputPath = Path.Combine(outputDir, $"pathtrace_{mode}_{DateTime.Now:yyyyMMdd_HHmmss}.png");

            bool exported = await Task.Run(() =>
                engineService.ExportPathTracedImage(
                    outputPath,
                    targetInstance));
            string message = exported ? $"Saved: {outputPath}" : "Path tracing export failed. Check Console panel for details.";

            await DisplayStatus("Path Tracing", message);
        }

        public Task SaveLayoutAsync()
        {
            return MauiProgram.GetService<EditorShellHost>().SaveLayoutAsync();
        }

        public Task ResetLayoutAsync()
        {
            return MauiProgram.GetService<EditorShellHost>().ResetLayoutAsync();
        }

        public async Task OpenSettingsAsync()
        {
            await MauiProgram.GetService<EditorShellHost>().OpenPanelAsync(KnownPanelTypes.Settings);
        }

        static Task DisplayStatus(string title, string message)
        {
            MauiProgram.GetService<EditorShellHost>().SetStatus(message);
            return Task.CompletedTask;
        }
    }
}
