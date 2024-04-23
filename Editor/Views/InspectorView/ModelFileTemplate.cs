using SailorEditor;
using SailorEditor.Helpers;
using SailorEditor.Services;
using SailorEditor.Utility;
using SailorEditor.ViewModels;

public class ModelFileTemplate : AssetFileTemplate
{
    public ModelFileTemplate()
    {
        LoadTemplate = () =>
        {
            var props = new Grid { ColumnDefinitions = { new ColumnDefinition { Width = GridLength.Auto }, new ColumnDefinition { Width = GridLength.Star } } };
            TemplateBuilder.AddGridRowWithLabel(props, "Generate materials", TemplateBuilder.CreateCheckBox(nameof(ModelFile.ShouldGenerateMaterials)), GridLength.Auto);
            TemplateBuilder.AddGridRowWithLabel(props, "Batch by material", TemplateBuilder.CreateCheckBox(nameof(ModelFile.ShouldBatchByMaterial)), GridLength.Auto);
            TemplateBuilder.AddGridRowWithLabel(props, "Unit Scale", TemplateBuilder.CreateEntry(nameof(ModelFile.UnitScale), new FloatValueConverter()), GridLength.Auto);

            var grid = new Grid { ColumnDefinitions = { new ColumnDefinition { Width = GridLength.Auto } } };
            TemplateBuilder.AddGridRow(grid, CreateControlPanel(), GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, new Label { Text = "Properties", FontAttributes = FontAttributes.Bold }, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, props, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, TemplateBuilder.CreateListEditor<Observable<string>>(nameof(ModelFile.DefaultMaterials), "Default Materials", "", new AssetUIDToFilenameConverter()), GridLength.Auto);

            return grid;
        };
    }
}