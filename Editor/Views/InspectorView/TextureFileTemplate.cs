using SailorEditor.Engine;
using SailorEditor.Helpers;
using SailorEditor.ViewModels;

public class TextureFileTemplate : DataTemplate
{
    public TextureFileTemplate()
    {
        LoadTemplate = () =>
        {
            var grid = new Grid
            {
                RowDefinitions =
                    {
                        new RowDefinition { Height = GridLength.Auto },
                        new RowDefinition { Height = GridLength.Auto },
                        new RowDefinition { Height = GridLength.Auto },
                        new RowDefinition { Height = GridLength.Auto },
                        new RowDefinition { Height = GridLength.Auto },
                        new RowDefinition { Height = GridLength.Auto },
                        new RowDefinition { Height = GridLength.Auto },
                        new RowDefinition { Height = GridLength.Auto },
                        new RowDefinition { Height = GridLength.Auto }
                    },
                ColumnDefinitions =
                    {
                        new ColumnDefinition { Width = GridLength.Auto },
                        new ColumnDefinition { Width = GridLength.Star }
                    }
            };

            var nameLabel = new Label { Text = "DisplayName" };
            nameLabel.SetBinding(Label.TextProperty, new Binding("DisplayName"));
            nameLabel.Behaviors.Add(new DisplayNameBehavior());

            var saveButton = new Button
            {
                Text = "Save",
                HorizontalOptions = LayoutOptions.Start,
                VerticalOptions = LayoutOptions.Start
            };
            saveButton.SetBinding(Button.IsVisibleProperty, new Binding("IsDirty"));
            saveButton.Clicked += (sender, e) => (saveButton.BindingContext as AssetFile).UpdateAssetFile();

            var revertButton = new Button
            {
                Text = "Revert",
                HorizontalOptions = LayoutOptions.End,
                VerticalOptions = LayoutOptions.Start
            };
            revertButton.SetBinding(Button.IsVisibleProperty, new Binding("IsDirty"));
            revertButton.Clicked += (sender, e) => (revertButton.BindingContext as AssetFile).Revert();

            var image = new Image
            {
                WidthRequest = 512,
                HeightRequest = 256,
                Aspect = Aspect.AspectFit,
                HorizontalOptions = LayoutOptions.Start,
                VerticalOptions = LayoutOptions.Start
            };
            image.SetBinding(Image.SourceProperty, new Binding("Texture"));

            var generateMipsCheckBox = TemplateBuilder.CreateCheckBox("ShouldGenerateMips");
            var storageBindingCheckBox = TemplateBuilder.CreateCheckBox("ShouldSupportStorageBinding");
            var filtrationPicker = TemplateBuilder.CreateEnumPicker<TextureFiltration>("Filtration");
            var clampingPicker = TemplateBuilder.CreateEnumPicker<TextureClamping>("Clamping");
            var formatPicker = TemplateBuilder.CreateEnumPicker<TextureFormat>("Format");

            TemplateBuilder.AddGridRow(grid, nameLabel, 0, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, saveButton, 1, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, revertButton, 1, GridLength.Auto);

            Grid.SetColumn(saveButton, 0);
            Grid.SetColumn(revertButton, 1);

            TemplateBuilder.AddGridRow(grid, image, 2, GridLength.Auto);
            Grid.SetColumnSpan(image, 2);

            TemplateBuilder.AddGridRowWithLabel(grid, "Generate Mips:", generateMipsCheckBox, 3);
            TemplateBuilder.AddGridRowWithLabel(grid, "Storage Binding:", storageBindingCheckBox, 4);
            TemplateBuilder.AddGridRowWithLabel(grid, "Filtration:", filtrationPicker, 5);
            TemplateBuilder.AddGridRowWithLabel(grid, "Clamping:", clampingPicker, 6);
            TemplateBuilder.AddGridRowWithLabel(grid, "Format:", formatPicker, 7);

            return grid;
        };
    }
}