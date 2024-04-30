using SailorEditor.Helpers;
using SailorEditor.ViewModels;

public class ShaderFileTemplate : AssetFileTemplate
{
    public ShaderFileTemplate()
    {
        LoadTemplate = () =>
        {
            var props = new Grid { ColumnDefinitions = { new ColumnDefinition { Width = GridLength.Auto }, new ColumnDefinition { Width = GridLength.Star } } };
            Templates.AddGridRowWithLabel(props, "Includes", Templates.ReadOnlyTextView(static (ShaderFile vm) => vm.Includes), GridLength.Auto);
            Templates.AddGridRowWithLabel(props, "Defines", Templates.ReadOnlyTextView(static (ShaderFile vm) => vm.Defines), GridLength.Auto);


            var grid = new Grid();
            Templates.AddGridRow(grid, CreateControlPanel(), GridLength.Auto);
            Templates.AddGridRow(grid, new Label { Text = "Properties", FontAttributes = FontAttributes.Bold }, GridLength.Auto);
            Templates.AddGridRow(grid, props, GridLength.Auto);
            Templates.AddGridRow(grid, new Label { Text = "Code", FontAttributes = FontAttributes.Bold }, GridLength.Auto);

            Templates.AddGridRow(grid, Templates.ShaderCodeView("Common Shader", static (ShaderFile vm) => vm.GlslCommonShader), GridLength.Auto);
            Templates.AddGridRow(grid, Templates.ShaderCodeView("Vertex Shader", static (ShaderFile vm) => vm.GlslVertexShader), GridLength.Auto);
            Templates.AddGridRow(grid, Templates.ShaderCodeView("Fragment Shader", static (ShaderFile vm) => vm.GlslFragmentShader), GridLength.Auto);
            Templates.AddGridRow(grid, Templates.ShaderCodeView("Compute Shader", static (ShaderFile vm) => vm.GlslComputeShader), GridLength.Auto);

            return grid;
        };
    }
}