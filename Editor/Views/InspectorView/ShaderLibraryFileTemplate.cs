using SailorEditor.Helpers;
using SailorEditor.ViewModels;
using System.Speech.Synthesis;

public class ShaderLibraryFileTemplate : AssetFileTemplate
{
    public ShaderLibraryFileTemplate()
    {
        LoadTemplate = () =>
        {
            var grid = new Grid { ColumnDefinitions = { new ColumnDefinition { Width = GridLength.Auto } } };

            TemplateBuilder.AddGridRow(grid, CreateControlPanel(), GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, new Label { Text = "Code", FontAttributes = FontAttributes.Bold }, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, TemplateBuilder.CreateReadOnlyEditor(nameof(ShaderLibraryFile.Code)), GridLength.Auto);

            return grid;
        };
    }
}