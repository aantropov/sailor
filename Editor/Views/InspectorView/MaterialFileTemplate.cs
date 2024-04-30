using CommunityToolkit.Maui.Converters;
using CommunityToolkit.Maui.Markup;
using SailorEditor.Engine;
using SailorEditor.Helpers;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using System.Numerics;
using BlendMode = SailorEditor.Engine.BlendMode;

public class MaterialFileTemplate : AssetFileTemplate
{
    public MaterialFileTemplate()
    {
        LoadTemplate = () =>
        {
            var grid = new Grid();
            var props = new Grid { ColumnDefinitions = { new ColumnDefinition { Width = GridLength.Auto }, new ColumnDefinition { Width = GridLength.Star } } };

            Templates.AddGridRowWithLabel(props, "Render Queue", Templates.EntryField(static (MaterialFile vm) => vm.RenderQueue,
                static (MaterialFile vm, string value) => vm.RenderQueue = value, null), GridLength.Auto);

            Templates.AddGridRowWithLabel(props, "Depth Bias", Templates.EntryField(static (MaterialFile vm) => vm.DepthBias,
                static (MaterialFile vm, float value) => vm.DepthBias = value, new FloatValueConverter()), GridLength.Auto);

            Templates.AddGridRowWithLabel(props, "Enable DepthTest", Templates.CheckBox(static (MaterialFile vm) => vm.EnableDepthTest, static (MaterialFile vm, bool value) => vm.EnableDepthTest = value), GridLength.Auto);
            Templates.AddGridRowWithLabel(props, "Enable ZWrite", Templates.CheckBox(static (MaterialFile vm) => vm.EnableZWrite, static (MaterialFile vm, bool value) => vm.EnableZWrite = value), GridLength.Auto);
            Templates.AddGridRowWithLabel(props, "Custom Depth", Templates.CheckBox(static (MaterialFile vm) => vm.CustomDepthShader, static (MaterialFile vm, bool value) => vm.CustomDepthShader = value), GridLength.Auto);
            Templates.AddGridRowWithLabel(props, "Support Multisampling", Templates.CheckBox(static (MaterialFile vm) => vm.SupportMultisampling, static (MaterialFile vm, bool value) => vm.SupportMultisampling = value), GridLength.Auto);

            Templates.AddGridRowWithLabel(props, "Blend mode", Templates.EnumPicker(static (MaterialFile vm) => vm.BlendMode, static (MaterialFile vm, BlendMode value) => vm.BlendMode = value), GridLength.Auto);
            Templates.AddGridRowWithLabel(props, "Cull mode", Templates.EnumPicker(static (MaterialFile vm) => vm.CullMode, static (MaterialFile vm, CullMode value) => vm.CullMode = value), GridLength.Auto);
            Templates.AddGridRowWithLabel(props, "Fill mode", Templates.EnumPicker(static (MaterialFile vm) => vm.FillMode, static (MaterialFile vm, FillMode value) => vm.FillMode = value), GridLength.Auto);

            Templates.AddGridRowWithLabel(props, "Shader", Templates.AssetUIDLabel(nameof(MaterialFile.Shader), static (MaterialFile vm) => vm.Shader, static (MaterialFile vm, AssetUID value) => vm.Shader = value), GridLength.Auto);

            Templates.AddGridRow(grid, CreateControlPanel(), GridLength.Auto);
            Templates.AddGridRow(grid, new Label { Text = "Properties", FontAttributes = FontAttributes.Bold }, GridLength.Auto);
            Templates.AddGridRow(grid, props, GridLength.Auto);
            Templates.AddGridRow(grid, Templates.CreateListEditor(static (MaterialFile vm) => vm.ShaderDefines,
                static (MaterialFile vm, ObservableList<Observable<string>> value) => vm.ShaderDefines = value,
                "Shader Defines",
                "NewDefine",
                converter: new ObservableConverter<string>()),
                GridLength.Auto);

            Templates.AddGridRow(grid, Templates.CreateUniformEditor<MaterialFile, Uniform<AssetUID>, AssetUID>(
                static (MaterialFile vm) => vm.Samplers,
                static (MaterialFile vm, ObservableList<Uniform<AssetUID>> value) => vm.Samplers = value,
                "Samplers", "newTextureSampler", new AssetUIDToFilenameConverter()),
                GridLength.Auto);

            Templates.AddGridRow(grid, Templates.CreateUniformEditor<MaterialFile, Uniform<Vec4>, Vec4>(
                static (MaterialFile vm) => vm.UniformsVec4,
                static (MaterialFile vm, ObservableList<Uniform<Vec4>> value) => vm.UniformsVec4 = value,
                "Uniforms Vec4", "material.newParam", new FloatValueConverter()),
                GridLength.Auto);

            Templates.AddGridRow(grid, Templates.CreateUniformEditor<MaterialFile, Uniform<float>, float>(
                static (MaterialFile vm) => vm.UniformsFloat,
                static (MaterialFile vm, ObservableList<Uniform<float>> value) => vm.UniformsFloat = value,
                "Uniforms Float", "material.newParam", new FloatValueConverter()),
                GridLength.Auto);

            //TemplateBuilder.AddGridRow(grid, TemplateBuilder.CreateDictionaryEditor<Observable<string>, Observable<string>>(nameof(MaterialFile.UniformsVec4), "Uniforms Vec4"), GridLength.Auto);

            return grid;
        };
    }
}