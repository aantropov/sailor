using SailorEditor.Services;
using System;
using System.Threading.Tasks;

namespace SailorEditor.Views;

public partial class CreateGameProjectPage : ContentPage
{
    public CreateGameProjectPage()
    {
        InitializeComponent();
    }

    private async void OnCreateClicked(object sender, EventArgs e)
    {
        var name = ProjectNameEntry.Text?.Trim();
        if (string.IsNullOrWhiteSpace(name))
        {
            await DisplayAlert("Error", "Project name cannot be empty", "OK");
            return;
        }

        var service = MauiProgram.GetService<GameProjectService>();
        service.CreateGameProject(name);

        await DisplayAlert("Game Project", $"Created project {name}", "OK");
        await Navigation.PopModalAsync();
    }
}
