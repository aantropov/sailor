using SailorEditor.ViewModels;
using SailorEditor.Services;
using System.ComponentModel;
using System.Globalization;
using BlendMode = SailorEditor.Engine.BlendMode;

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
            if (item is TextureFile)
                return TextureFileTemplate;

            if (item is ShaderFile)
                return ShaderFileTemplate;

            if (item is ShaderLibraryFile)
                return ShaderLibraryFileTemplate;

            if (item is ModelFile)
                return ModelFileTemplate;

            if (item is MaterialFile)
                return MaterialFileTemplate;

            return AssetFileTemplate;
        }
    }
}