using SailorEditor.ViewModels;

namespace SailorEditor;
public class InspectorTemplateSelector : DataTemplateSelector
{
    public DataTemplate TextureFileTemplate { get; set; }
    public DataTemplate AssetFileTemplate { get; set; }
    public DataTemplate ModelFileTemplate { get; set; }
    public DataTemplate ShaderFileTemplate { get; set; }
    public DataTemplate ShaderLibraryFileTemplate { get; set; }
    public DataTemplate MaterialFileTemplate { get; set; }
    public DataTemplate GameObjectTemplate { get; set; }

    protected override DataTemplate OnSelectTemplate(object item, BindableObject container)
    {
        DataTemplate template = item switch
        {
            TextureFile => TextureFileTemplate,
            ShaderFile => ShaderFileTemplate,
            ShaderLibraryFile => ShaderLibraryFileTemplate,
            ModelFile => ModelFileTemplate,
            MaterialFile => MaterialFileTemplate,
            _ => AssetFileTemplate
        };

        return template;
    }
}
