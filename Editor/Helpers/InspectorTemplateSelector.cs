using Editor.ViewModels;
using Editor.Services;
using Microsoft.Maui.Controls;
using System.ComponentModel;

namespace Editor.Helpers
{
    public class InspectorTemplateSelector : DataTemplateSelector
    {
        public DataTemplate Default { get; set; }
        public DataTemplate AssetFileTexture { get; set; }

        public InspectorTemplateSelector()
        {
            Default = new DataTemplate(() =>
            {
                var label = new Label();
                label.SetBinding(Label.TextProperty, new Binding("Name"));
                label.TextColor = Color.FromRgb(255, 0, 0);
                return label;
            });

            AssetFileTexture = new DataTemplate(() =>
            {
                var label = new Label();
                label.SetBinding(Label.TextProperty, new Binding("Name"));
                label.TextColor = Color.FromRgb(0, 255, 0);
                return label;
            });
        }
        protected override DataTemplate OnSelectTemplate(object item, BindableObject container)
        {
            var assetFile = (AssetFile)item;

            if (assetFile != null)
            {
                if (assetFile.Name.EndsWith(".png"))
                {
                    return AssetFileTexture;
                }
            }

            return Default;
        }
    }
}