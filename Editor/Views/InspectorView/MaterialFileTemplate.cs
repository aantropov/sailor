using SailorEditor.Helpers;
using SailorEditor.Utility;
using SailorEngine;
using SailorEditor.ViewModels;
using BlendMode = SailorEngine.BlendMode;

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

            Templates.AddGridRowWithLabel(props, "Shader", Templates.AssetUIDLabel(nameof(MaterialFile.Shader), static (MaterialFile vm) => vm.Shader, static (MaterialFile vm, FileId value) => vm.Shader = value), GridLength.Auto);

            Templates.AddGridRow(grid, CreateControlPanel(), GridLength.Auto);
            Templates.AddGridRow(grid, new Label { Text = "Properties", FontAttributes = FontAttributes.Bold }, GridLength.Auto);
            Templates.AddGridRow(grid, props, GridLength.Auto);
            Templates.AddGridRow(grid, Templates.ListEditor(
                static (MaterialFile vm) => vm.ShaderDefines,
                static (MaterialFile vm, ObservableList<Observable<string>> value) => vm.ShaderDefines = value,
                () => Templates.EntryField(static (Observable<string> vm) => vm.Value,
                static (Observable<string> vm, string value) => vm.Value = value),
                "Shader Defines",
                "NewDefine",
                converter: new ObservableConverter<string>()),
                GridLength.Auto);

            Templates.AddGridRow(grid, Templates.UniformEditor(
                static (MaterialFile vm) => vm.Samplers,
                static (MaterialFile vm, ObservableList<Uniform<FileId>> value) => vm.Samplers = value,
                () => Templates.TextureEditor(static (Uniform<FileId> vm) => vm.Value, static (Uniform<FileId> vm, FileId value) => vm.Value = value),
                "Samplers", "newTextureSampler"),
                GridLength.Auto);

            Templates.AddGridRow(grid, Templates.UniformEditor(
                static (MaterialFile vm) => vm.UniformsVec4,
                static (MaterialFile vm, ObservableList<Uniform<SailorEditor.Vec4>> value) => vm.UniformsVec4 = value,
                () => Templates.Vec4Editor(static (Uniform<SailorEditor.Vec4> vm) => vm.Value),
                "Uniforms Vec4", "material.newVec4Param", new SailorEditor.Vec4()),
                GridLength.Auto);

            Templates.AddGridRow(grid, Templates.UniformEditor(
                static (MaterialFile vm) => vm.UniformsFloat,
                static (MaterialFile vm, ObservableList<Uniform<float>> value) => vm.UniformsFloat = value,
                () => Templates.FloatEditor(static (Uniform<float> vm) => vm.Value, static (Uniform<float> vm, float value) => vm.Value = value),
                "Uniforms Float", "material.newFloatParam"),
                GridLength.Auto);

            return grid;
        };
    }
}