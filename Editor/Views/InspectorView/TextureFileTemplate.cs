using Microsoft.Maui.Controls.Compatibility;
using SailorEditor.Engine;
using SailorEditor.Helpers;
using SailorEditor.ViewModels;
using System.Speech.Synthesis;
using Grid = Microsoft.Maui.Controls.Grid;

public class TextureFileTemplate : AssetFileTemplate
{
    public TextureFileTemplate()
    {
        LoadTemplate = () =>
        {
            var grid = new Grid { ColumnDefinitions = { new ColumnDefinition { Width = GridLength.Auto } } };

            var image = new Image
            {
                WidthRequest = 512,
                HeightRequest = 256,
                Aspect = Aspect.AspectFit,
                HorizontalOptions = LayoutOptions.Start,
                VerticalOptions = LayoutOptions.Start
            };

            image.SetBinding(Image.SourceProperty, new Binding("Texture"));
            image.SetBinding(Image.IsVisibleProperty, new Binding("IsImageLoaded"));

            var props = new Grid { ColumnDefinitions = { new ColumnDefinition { Width = GridLength.Auto }, new ColumnDefinition { Width = GridLength.Star } } };

            TemplateBuilder.AddGridRowWithLabel(props, "Generate Mips", TemplateBuilder.CreateCheckBox(nameof(TextureFile.ShouldGenerateMips)), GridLength.Auto);
            TemplateBuilder.AddGridRowWithLabel(props, "Storage Binding", TemplateBuilder.CreateCheckBox(nameof(TextureFile.ShouldSupportStorageBinding)), GridLength.Auto);
            TemplateBuilder.AddGridRowWithLabel(props, "Filtration", TemplateBuilder.CreateEnumPicker<TextureFiltration>(nameof(TextureFile.Filtration)), GridLength.Auto);
            TemplateBuilder.AddGridRowWithLabel(props, "Clamping", TemplateBuilder.CreateEnumPicker<TextureClamping>(nameof(TextureFile.Clamping)), GridLength.Auto);
            TemplateBuilder.AddGridRowWithLabel(props, "Format", TemplateBuilder.CreateEnumPicker<TextureFormat>(nameof(TextureFile.Format)), GridLength.Auto);

            TemplateBuilder.AddGridRow(grid, CreateControlPanel(), GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, image, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, new Label { Text = "Properties", FontAttributes = FontAttributes.Bold }, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, props, GridLength.Auto);

            return grid;
        };
    }
}