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

            Templates.AddGridRow(grid, CreateControlPanel(), GridLength.Auto);
            Templates.AddGridRow(grid, new Label { Text = "Code", FontAttributes = FontAttributes.Bold }, GridLength.Auto);
            Templates.AddGridRow(grid, Templates.ReadOnlyTextView(static (ShaderLibraryFile vm) => vm.Code), GridLength.Auto);

            return grid;
        };
    }
}