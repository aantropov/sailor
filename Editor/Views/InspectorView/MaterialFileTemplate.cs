using CommunityToolkit.Maui.Converters;
using SailorEditor.Engine;
using SailorEditor.Helpers;
using SailorEditor.Utility;
using SailorEditor.ViewModels;

public class MaterialFileTemplate : AssetFileTemplate
{
    public MaterialFileTemplate()
    {
        LoadTemplate = () =>
        {
            var grid = new Grid();
            var props = new Grid { ColumnDefinitions = { new ColumnDefinition { Width = GridLength.Auto }, new ColumnDefinition { Width = GridLength.Star } } };

            var shaderLabel = new Label();
            shaderLabel.SetBinding(Label.TextProperty, new Binding(nameof(MaterialFile.Shader), BindingMode.Default, new AssetUIDToFilenameConverter()));
            shaderLabel.Behaviors.Add(new AssetUIDClickable(nameof(MaterialFile.Shader)));

            TemplateBuilder.AddGridRowWithLabel(props, "Render Queue", TemplateBuilder.CreateEntry(nameof(MaterialFile.RenderQueue)), GridLength.Auto);
            TemplateBuilder.AddGridRowWithLabel(props, "Depth Bias", TemplateBuilder.CreateEntry(nameof(MaterialFile.DepthBias)), GridLength.Auto);

            TemplateBuilder.AddGridRowWithLabel(props, "Enable DepthTest", TemplateBuilder.CreateCheckBox(nameof(MaterialFile.EnableDepthTest)), GridLength.Auto);
            TemplateBuilder.AddGridRowWithLabel(props, "Enable ZWrite", TemplateBuilder.CreateCheckBox(nameof(MaterialFile.EnableZWrite)), GridLength.Auto);
            TemplateBuilder.AddGridRowWithLabel(props, "Custom Depth", TemplateBuilder.CreateCheckBox(nameof(MaterialFile.CustomDepthShader)), GridLength.Auto);
            TemplateBuilder.AddGridRowWithLabel(props, "Support Multisampling", TemplateBuilder.CreateCheckBox(nameof(MaterialFile.SupportMultisampling)), GridLength.Auto);

            TemplateBuilder.AddGridRowWithLabel(props, "Blend mode", TemplateBuilder.CreateEnumPicker<SailorEditor.Engine.BlendMode>(nameof(MaterialFile.BlendMode)), GridLength.Auto);
            TemplateBuilder.AddGridRowWithLabel(props, "Cull mode", TemplateBuilder.CreateEnumPicker<CullMode>(nameof(MaterialFile.CullMode)), GridLength.Auto);
            TemplateBuilder.AddGridRowWithLabel(props, "Fill mode", TemplateBuilder.CreateEnumPicker<FillMode>(nameof(MaterialFile.FillMode)), GridLength.Auto);

            TemplateBuilder.AddGridRowWithLabel(props, "Shader", shaderLabel, GridLength.Auto);

            TemplateBuilder.AddGridRow(grid, CreateControlPanel(), GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, new Label { Text = "Properties", FontAttributes = FontAttributes.Bold }, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, props, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, TemplateBuilder.CreateListEditor(nameof(MaterialFile.ShaderDefines),
                "Shader Defines",
                new Observable<string>("NewDefine"),
                new ObservableConverter<string>()),
                GridLength.Auto);

            TemplateBuilder.AddGridRow(grid, TemplateBuilder.CreateUniformEditor<string>(nameof(MaterialFile.Samplers),
                "Samplers", "newTextureSampler", new AssetUIDToFilenameConverter()),
                GridLength.Auto);

            TemplateBuilder.AddGridRow(grid, TemplateBuilder.CreateUniformEditor<float>(nameof(MaterialFile.UniformsFloat),
                "Uniforms Float", "material.newParam", new FloatValueConverter()),
                GridLength.Auto);

            //TemplateBuilder.AddGridRow(grid, TemplateBuilder.CreateDictionaryEditor<Observable<string>, Observable<string>>(nameof(MaterialFile.UniformsVec4), "Uniforms Vec4"), GridLength.Auto);

            return grid;
        };
    }
}