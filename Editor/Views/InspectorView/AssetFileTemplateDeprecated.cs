using SailorEditor;
using SailorEditor.Helpers;
using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEditor.Views;

public class AssetFileTemplateDeprecated : DataTemplate
{
    public AssetFileTemplateDeprecated()
    {
        LoadTemplate = () =>
        {
            var label = new Label();
            label.SetBinding(Label.TextProperty, new Binding("DisplayName"));
            label.Behaviors.Add(new DisplayNameBehavior());
            label.TextColor = Color.FromRgb(255, 0, 0);

            return label;
        };
    }
    protected View CreateControlPanel()
    {
        var controlPanel = new Grid
        {
            ColumnDefinitions = { new ColumnDefinition { Width = GridLength.Auto } },
            ColumnSpacing = 10,
            RowSpacing = 5,
            Padding = new Thickness(5)
        };

        var nameLabel = new Label { Text = "DisplayName", VerticalOptions = LayoutOptions.Center, HorizontalTextAlignment = TextAlignment.Start, FontAttributes = FontAttributes.Bold };
        nameLabel.SetBinding(Label.TextProperty, new Binding("DisplayName"));
        nameLabel.Behaviors.Add(new DisplayNameBehavior());

        var openButton = new Button
        {
            Text = "Open",
            HorizontalOptions = LayoutOptions.Start,
            VerticalOptions = LayoutOptions.Start,
            HeightRequest = 30,
            WidthRequest = 80
        };

        var saveButton = new Button
        {
            Text = "Save",
            HorizontalOptions = LayoutOptions.Start,
            VerticalOptions = LayoutOptions.Start,
            HeightRequest = 30,
            WidthRequest = 80
        };

        var revertButton = new Button
        {
            Text = "Revert",
            HorizontalOptions = LayoutOptions.Start,
            VerticalOptions = LayoutOptions.Start,
            HeightRequest = 10,
            WidthRequest = 80
        };

        saveButton.SetBinding(Button.IsVisibleProperty, new Binding("IsDirty"));
        revertButton.SetBinding(Button.IsVisibleProperty, new Binding("IsDirty"));
        openButton.SetBinding(Button.IsVisibleProperty, new Binding("CanOpenAssetFile"));

        openButton.Clicked += (sender, e) => (saveButton.BindingContext as AssetFile).Open();
        saveButton.Clicked += async (sender, e) =>
        {
            _ = (saveButton.BindingContext as AssetFile).Save();
        };

        revertButton.Clicked += async (sender, e) => await (revertButton.BindingContext as AssetFile).Revert();

        Templates.AddGridRow(controlPanel, nameLabel, GridLength.Auto);

        var buttonsStack = new HorizontalStackLayout
        {
            Children = { openButton, saveButton, revertButton },
            Spacing = 5,
            HorizontalOptions = LayoutOptions.Start,
            VerticalOptions = LayoutOptions.Start
        };

        Templates.AddGridRow(controlPanel, buttonsStack, GridLength.Auto);

        controlPanel.HorizontalOptions = LayoutOptions.Start;
        controlPanel.VerticalOptions = LayoutOptions.Start;

        return controlPanel;
    }
}
