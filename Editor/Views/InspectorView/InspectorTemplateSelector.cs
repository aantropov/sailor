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
    public DataTemplate FrameGraphFileTemplate { get; set; }
    public DataTemplate WorldFileTemplate { get; set; }
    public DataTemplate AnimationControllerFileTemplate { get; set; }
    public DataTemplate AnimationSetFileTemplate { get; set; }
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
            FrameGraphFile => FrameGraphFileTemplate,
            AnimationControllerFile => AnimationControllerFileTemplate,
            AnimationSetFile => AnimationSetFileTemplate,
            AnimationFile => AssetFileTemplate,
            PrefabFile => AssetFileTemplate,
            WorldFile => WorldFileTemplate,
            GameObject => GameObjectTemplate,
            _ => AssetFileTemplate
        };

        return template;
    }
}

public class ComponentTemplateSelector : DataTemplateSelector
{
    public DataTemplate ComponentTemplate { get; set; }

    protected override DataTemplate OnSelectTemplate(object item, BindableObject container)
    {
        return ComponentTemplate;
    }
}
