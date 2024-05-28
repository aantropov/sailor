using SailorEditor.Helpers;
using SailorEditor.Engine;
using SailorEditor.ViewModels;
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

            Templates.AddGridRowWithLabel(props, "Generate Mips", Templates.CheckBox(static (TextureFile vm) => vm.ShouldGenerateMips, static (TextureFile vm, bool value) => vm.ShouldGenerateMips = value), GridLength.Auto);
            Templates.AddGridRowWithLabel(props, "Storage Binding", Templates.CheckBox(static (TextureFile vm) => vm.ShouldSupportStorageBinding, static (TextureFile vm, bool value) => vm.ShouldSupportStorageBinding = value), GridLength.Auto);
            Templates.AddGridRowWithLabel(props, "Filtration", Templates.EnumPicker(static (TextureFile vm) => vm.Filtration, static (TextureFile vm, TextureFiltration value) => vm.Filtration = value), GridLength.Auto);
            Templates.AddGridRowWithLabel(props, "Clamping", Templates.EnumPicker(static (TextureFile vm) => vm.Clamping, static (TextureFile vm, TextureClamping value) => vm.Clamping = value), GridLength.Auto);
            Templates.AddGridRowWithLabel(props, "Format", Templates.EnumPicker(static (TextureFile vm) => vm.Format, static (TextureFile vm, TextureFormat value) => vm.Format = value), GridLength.Auto);

            Templates.AddGridRow(grid, CreateControlPanel(), GridLength.Auto);
            Templates.AddGridRow(grid, image, GridLength.Auto);
            Templates.AddGridRow(grid, new Label { Text = "Properties", FontAttributes = FontAttributes.Bold }, GridLength.Auto);
            Templates.AddGridRow(grid, props, GridLength.Auto);

            return grid;
        };
    }
}