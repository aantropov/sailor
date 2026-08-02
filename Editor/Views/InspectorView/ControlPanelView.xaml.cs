using Microsoft.Maui.Controls;
using Microsoft.Maui.Controls.Xaml;
using SailorEditor.Services;
using SailorEditor.ViewModels;
using System;

namespace SailorEditor.Views;

[XamlCompilation(XamlCompilationOptions.Compile)]
public partial class ControlPanelView : ContentView
{
    public ControlPanelView()
    {
        InitializeComponent();
    }

    private void OnOpenButtonClicked(object sender, EventArgs e)
    {
        var assetFile = (sender as Button)?.BindingContext as AssetFile;
        assetFile?.Open();
    }

    private async void OnSaveButtonClicked(object sender, EventArgs e)
    {
        if (sender is Button { BindingContext: AssetFile assetFile })
        {
            var assetsService = MauiProgram.GetService<AssetsService>();
            if (!await assetsService.SaveAssetAsync(assetFile))
            {
                MauiProgram.GetService<EngineService>().PushConsoleMessage(
                    $"Asset was saved, but its targeted native update did not commit: {assetFile.AssetInfo.FullName}");
            }
        }
    }

    private async void OnRevertButtonClicked(object sender, EventArgs e)
    {
        if (sender is Button { BindingContext: AssetFile assetFile })
        {
            await assetFile.Revert();
        }
    }
}
