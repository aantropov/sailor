using SailorEditor.Commands;
using SailorEditor.Panels;
using SailorEditor.Services;
using SailorEditor.Shell;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using System;
using System.Linq;
using SailorEngine;
using YamlDotNet.Serialization.NamingConventions;
using YamlDotNet.Serialization;
using System.Diagnostics;
using ViewModelComponent = SailorEditor.ViewModels.Component;
using ViewModelGameObject = SailorEditor.ViewModels.GameObject;

namespace SailorEditor.Views
{
    public partial class ToolbarView : ContentView
    {
        public ToolbarView()
        {
            InitializeComponent();
            BindingContext = this;
        }

        private async void OnSaveButtonClicked(object sender, EventArgs e)
        {
            var selectionService = MauiProgram.GetService<SelectionService>();
            var selected = selectionService.SelectedItem;

            switch (selected)
            {
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

            var dirtyAsset = MauiProgram.GetService<AssetsService>().Files.FirstOrDefault(x => x.IsDirty);
            if (dirtyAsset != null)
            {
                await dirtyAsset.Save();
                await DisplayStatus("Save", $"Saved {dirtyAsset.DisplayName}");
                return;
            }

            await DisplayStatus("Save", "Nothing to save.");
        }

        private async void OnUndoButtonClicked(object sender, EventArgs e) => await MauiProgram.GetService<ICommandHistoryService>().UndoAsync(new CommandOrigin(CommandOriginKind.UI, "ToolbarUndo"));
        private async void OnRedoButtonClicked(object sender, EventArgs e) => await MauiProgram.GetService<ICommandHistoryService>().RedoAsync(new CommandOrigin(CommandOriginKind.UI, "ToolbarRedo"));
        private void OnPlayButtonClicked(object sender, EventArgs e) => RunWorld(false);
        private void OnPlayDebugButtonClicked(object sender, EventArgs e) => RunWorld(true);
        private async void OnPathTraceSceneButtonClicked(object sender, EventArgs e) => await ExportPathTracedImage(false);
        private async void OnPathTraceSelectionButtonClicked(object sender, EventArgs e) => await ExportPathTracedImage(true);
        private async void OnPanelsButtonClicked(object sender, EventArgs e) => await OpenPanelPickerAsync();
        private async void OnSettingsButtonClicked(object sender, EventArgs e) => await MauiProgram.GetService<EditorShellHost>().OpenPanelAsync(KnownPanelTypes.Settings);

        void RunWorld(bool bDebug)
        {
            var worldService = MauiProgram.GetService<WorldService>();
            var engineService = MauiProgram.GetService<EngineService>();

            Task.Run(() =>
            {
                string world = worldService.SerializeCurrentWorld();

                using (var yamlAssetInfo = new FileStream(engineService.EngineCacheDirectory + "\\Temp.world", FileMode.OpenOrCreate))
                using (var writer = new StreamWriter(yamlAssetInfo))
                {
                    writer.Write(world);
                }

                engineService.RunWorld("../Cache/Temp.world", bDebug);
            });
        }

        async Task OpenPanelPickerAsync()
        {
            var shellHost = MauiProgram.GetService<EditorShellHost>();
            var registry = MauiProgram.GetService<PanelRegistry>();
            var page = Application.Current?.Windows?.FirstOrDefault()?.Page;

            if (page is null)
                return;

            var descriptors = registry.GetAllDescriptors().OrderBy(x => x.Role).ThenBy(x => x.Title).ToArray();
            var orderedButtons = descriptors.Select(x => x.Title).ToArray();
            var picked = await page.DisplayActionSheet("Open panel", "Cancel", null, orderedButtons);

            if (string.IsNullOrWhiteSpace(picked) || picked == "Cancel")
                return;

            var descriptor = descriptors.FirstOrDefault(x => x.Title == picked);
            if (descriptor is not null)
                await shellHost.OpenPanelAsync(descriptor.TypeId);
        }

        async Task ExportPathTracedImage(bool bSelectedOnly)
        {
            var engineService = MauiProgram.GetService<EngineService>();
            var selectionService = MauiProgram.GetService<SelectionService>();

            InstanceId targetInstance = null;
            if (bSelectedOnly)
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

            var mode = bSelectedOnly ? "selection" : "scene";
            var outputPath = Path.Combine(outputDir, $"pathtrace_{mode}_{DateTime.Now:yyyyMMdd_HHmmss}.png");

            bool bExported = engineService.ExportPathTracedImage(outputPath, targetInstance);
            string message = bExported ? $"Saved: {outputPath}" : "Path tracing export failed. Check Console panel for details.";

            await DisplayStatus("Path Tracing", message);
        }

        static Task DisplayStatus(string title, string message)
        {
            var page = Application.Current?.Windows?.FirstOrDefault()?.Page;
            return page != null ? page.DisplayAlert(title, message, "OK") : Task.CompletedTask;
        }
    }
}
