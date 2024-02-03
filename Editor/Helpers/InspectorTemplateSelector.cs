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
                var stackLayout = new VerticalStackLayout();

                var label = new Label();
                //label.SetBinding(Label.TextProperty, new Binding("Name", BindingMode.Default, null, null, "{0} (Type: Texture)"));

                var image = new Image();
                image.SetBinding(Image.SourceProperty, new Binding("ImageSource"));
                image.Aspect = Aspect.Center;

                stackLayout.Children.Add(label);
                stackLayout.Children.Add(image);
                
                return stackLayout;
            });
        }
        protected override DataTemplate OnSelectTemplate(object item, BindableObject container)
        {
            var assetFile = item as TextureFile;

            if (assetFile != null)
            {
                return AssetFileTexture;
            }

            return Default;
        }
    }
}