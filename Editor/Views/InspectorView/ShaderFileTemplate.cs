using SailorEditor.Helpers;
using SailorEditor.ViewModels;

public class ShaderFileTemplate : AssetFileTemplate
{
    public ShaderFileTemplate()
    {
        LoadTemplate = () =>
        {
            var props = new Grid { ColumnDefinitions = { new ColumnDefinition { Width = GridLength.Auto }, new ColumnDefinition { Width = GridLength.Star } } };
            TemplateBuilder.AddGridRowWithLabel(props, "Includes", TemplateBuilder.CreateReadOnlyEditor(nameof(ShaderFile.Includes)), GridLength.Auto);
            TemplateBuilder.AddGridRowWithLabel(props, "Defines", TemplateBuilder.CreateReadOnlyEditor(nameof(ShaderFile.Defines)), GridLength.Auto);


            var grid = new Grid();
            TemplateBuilder.AddGridRow(grid, CreateControlPanel(), GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, new Label { Text = "Properties", FontAttributes = FontAttributes.Bold }, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, props, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, new Label { Text = "Code", FontAttributes = FontAttributes.Bold }, GridLength.Auto);

            TemplateBuilder.AddGridRow(grid, TemplateBuilder.CreateShaderCodeView("Common Shader", nameof(ShaderFile.GlslCommonShader)), GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, TemplateBuilder.CreateShaderCodeView("Vertex Shader", nameof(ShaderFile.GlslVertexShader)), GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, TemplateBuilder.CreateShaderCodeView("Fragment Shader", nameof(ShaderFile.GlslFragmentShader)), GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, TemplateBuilder.CreateShaderCodeView("Compute Shader", nameof(ShaderFile.GlslComputeShader)), GridLength.Auto);

            return grid;
        };
    }
}