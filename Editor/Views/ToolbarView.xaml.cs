using SailorEditor.Services;
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

        private void OnSaveButtonClicked(object sender, EventArgs e)
        {
        }

        private void OnPlayButtonClicked(object sender, EventArgs e) => RunWorld(false);
        private void OnPlayDebugButtonClicked(object sender, EventArgs e) => RunWorld(true);
        private async void OnPathTraceSceneButtonClicked(object sender, EventArgs e) => await ExportPathTracedImage(false);
        private async void OnPathTraceSelectionButtonClicked(object sender, EventArgs e) => await ExportPathTracedImage(true);

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
