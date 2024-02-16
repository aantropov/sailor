using SailorEditor.Helpers;

public class ShaderFileTemplate : DataTemplate
{
    public ShaderFileTemplate()
    {
        LoadTemplate = () =>
        {
            var grid = new Grid
            {
                RowDefinitions =
                    {
                        new RowDefinition { Height = GridLength.Auto }, // Name
                        new RowDefinition { Height = GridLength.Auto }, // Includes
                        new RowDefinition { Height = GridLength.Auto }, // Defines
                        new RowDefinition { Height = GridLength.Auto }, // Defines
                        new RowDefinition { Height = GridLength.Auto }, // Common Shader
                        new RowDefinition { Height = GridLength.Auto }, // Vertex Shader
                        new RowDefinition { Height = GridLength.Auto }, // Fragment Shader
                        new RowDefinition { Height = GridLength.Auto }  // Compute Shader
                    }
            };

            var nameLabel = new Label { Text = "DisplayName" };
            nameLabel.SetBinding(Label.TextProperty, new Binding("DisplayName"));
            nameLabel.Behaviors.Add(new DisplayNameBehavior());

            var includesLabel = new Label { Text = "Includes:" };
            var includesList = TemplateBuilder.CreateReadOnlyEditor("Includes");
            var definesLabel = new Label { Text = "Defines:" };
            var definesList = TemplateBuilder.CreateReadOnlyEditor("Defines");

            var commonShaderExpander = TemplateBuilder.CreateShaderCodeView("Common Shader", "GlslCommonShader");
            var vertexShaderExpander = TemplateBuilder.CreateShaderCodeView("Vertex Shader", "GlslVertexShader");
            var fragmentShaderExpander = TemplateBuilder.CreateShaderCodeView("Fragment Shader", "GlslFragmentShader");
            var computeShaderExpander = TemplateBuilder.CreateShaderCodeView("Compute Shader", "GlslComputeShader");

            TemplateBuilder.AddGridRow(grid, nameLabel, 0, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, includesLabel, 1, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, includesList, 2, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, definesLabel, 3, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, definesList, 4, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, commonShaderExpander, 5, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, vertexShaderExpander, 6, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, fragmentShaderExpander, 7, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, computeShaderExpander, 8, GridLength.Auto);

            return grid;
        };
    }
}