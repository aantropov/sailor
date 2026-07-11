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

            switch (selected)
            {
                case AssetFile { IsReadOnly: true } readOnlyAsset:
                    await DisplayStatus("Save", $"{readOnlyAsset.DisplayName} belongs to read-only engine content.");
                    return;

                case AssetFile assetFile when assetFile.IsDirty:
                    await Task.Yield();
                    await assetFile.Save();
                    await DisplayStatus("Save", $"Saved {assetFile.DisplayName}");
                    return;

                case IInspectorEditable editable when editable.HasPendingInspectorChanges:
                    if (editable.CommitInspectorChanges())
                    {
                        await DisplayStatus("Save", "Committed inspector changes.");
                    }
                    else
                    {
                        await DisplayStatus("Save", "Nothing was committed.");
                    }
                    return;

                case AssetFile assetFile:
                    await DisplayStatus("Save", $"{assetFile.DisplayName} has no pending changes.");
                    return;
            }

            var dirtyAsset = MauiProgram.GetService<AssetsService>().Files.FirstOrDefault(x => x.IsDirty && !x.IsReadOnly);
            if (dirtyAsset != null)
            {
                await dirtyAsset.Save();
                await DisplayStatus("Save", $"Saved {dirtyAsset.DisplayName}");
                return;
            }

            await DisplayStatus("Save", "Nothing to save.");
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

                engineService.RunWorld("../Cache/Temp.world", debug, launchContext);
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

            bool exported = engineService.ExportPathTracedImage(outputPath, targetInstance);
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
