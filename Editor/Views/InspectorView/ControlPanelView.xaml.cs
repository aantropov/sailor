using Microsoft.Maui.Controls;
using Microsoft.Maui.Controls.Xaml;
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
            await assetFile.Save();
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
