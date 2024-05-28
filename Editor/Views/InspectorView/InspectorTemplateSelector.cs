using SailorEditor.ViewModels;

namespace SailorEditor.Helpers
{
    public class InspectorTemplateSelector : DataTemplateSelector
    {
        public DataTemplate AssetFileTemplate { get; private set; } = new AssetFileTemplate();
        public DataTemplate TextureFileTemplate { get; private set; } = new TextureFileTemplate();
        public DataTemplate ModelFileTemplate { get; private set; } = new ModelFileTemplate();
        public DataTemplate ShaderFileTemplate { get; private set; } = new ShaderFileTemplate();
        public DataTemplate ShaderLibraryFileTemplate { get; private set; } = new ShaderLibraryFileTemplate();
        public DataTemplate MaterialFileTemplate { get; private set; } = new MaterialFileTemplate();

        protected override DataTemplate OnSelectTemplate(object item, BindableObject container)
        {
            switch (item)
            {
                case TextureFile _:
                    return TextureFileTemplate;

                case ShaderFile _:
                    return ShaderFileTemplate;

                case ShaderLibraryFile _:
                    return ShaderLibraryFileTemplate;

                case ModelFile _:
                    return ModelFileTemplate;

                case MaterialFile _:
                    return MaterialFileTemplate;
            };

            return AssetFileTemplate;
        }
    }
}