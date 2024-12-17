using SailorEditor.Services;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using System;
using YamlDotNet.Serialization.NamingConventions;
using YamlDotNet.Serialization;
using System.Diagnostics;

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
    }
}