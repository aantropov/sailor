using SailorEditor;
using SailorEditor.Helpers;
using SailorEditor.Services;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEngine;


public class GameObjectTemplate : DataTemplate
{
    public GameObjectTemplate()
    {
        LoadTemplate = () =>
        {
            var grid = new Grid();
            var props = new Grid { ColumnDefinitions = { new ColumnDefinition { Width = GridLength.Auto }, new ColumnDefinition { Width = GridLength.Star } } };

            Templates.AddGridRow(grid, CreateControlPanel(), GridLength.Auto);
            Templates.AddGridRow(grid, new Label { Text = "Transform", FontAttributes = FontAttributes.Bold }, GridLength.Auto);
            
            Templates.AddGridRowWithLabel(props, "Position", Templates.Vec4Editor(static (GameObject vm) => vm.Position), GridLength.Auto);
            Templates.AddGridRowWithLabel(props, "Rotation", Templates.Vec4Editor(static (GameObject vm) => vm.Rotation), GridLength.Auto);
            Templates.AddGridRowWithLabel(props, "Scale", Templates.Vec4Editor(static (GameObject vm) => vm.Scale), GridLength.Auto);

            Templates.AddGridRow(grid, props, GridLength.Auto);
            Templates.AddGridRow(grid, new Label { Text = "Components", FontAttributes = FontAttributes.Bold }, GridLength.Auto);

            var componentsStack = new VerticalStackLayout();
            componentsStack.SetBinding(BindableLayout.ItemsSourceProperty, new Binding("Components"));

            BindableLayout.SetItemTemplate(componentsStack, new DataTemplate(() =>
            {
                var componentEditor = new ContentView();
                componentEditor.SetBinding(ContentView.BindingContextProperty, new Binding("."));
                componentEditor.Content = Templates.ComponentEditor();

                return componentEditor;
            }));

            Templates.AddGridRow(grid, componentsStack, GridLength.Star);

            return grid;
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
        nameLabel.SetBinding(Label.TextProperty, new Binding("Name"));
        nameLabel.Behaviors.Add(new DisplayNameBehavior());

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

        saveButton.Clicked += async (sender, e) => await (saveButton.BindingContext as AssetFile).UpdateAssetFile();
        revertButton.Clicked += async (sender, e) => await (revertButton.BindingContext as AssetFile).Revert();

        Templates.AddGridRow(controlPanel, nameLabel, GridLength.Auto);

        var buttonsStack = new HorizontalStackLayout
        {
            Children = { saveButton, revertButton },
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
