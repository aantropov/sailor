using SailorEditor.ViewModels;
using SailorEditor.Services;
using Microsoft.Maui.Controls;
using System.ComponentModel;
using SailorEditor.Engine;
using System.Globalization;

namespace SailorEditor.Helpers
{
    public class InspectorTemplateSelector : DataTemplateSelector
    {
        public DataTemplate Default { get; private set; }
        public DataTemplate AssetFileTexture { get; private set; }
        public DataTemplate AssetFileShader { get; private set; }
        public DataTemplate AssetFileShaderLibrary { get; private set; }

        public InspectorTemplateSelector()
        {
            Default = new DataTemplate(() =>
            {
                var label = new Label();
                label.SetBinding(Label.TextProperty, new Binding("Name"));
                label.TextColor = Color.FromRgb(255, 0, 0);
                return label;
            });

            AssetFileShaderLibrary = new DataTemplate(() =>
            {
                var grid = new Grid
                {
                    RowDefinitions =
                    {
                        new RowDefinition { Height = GridLength.Auto }, // Name
                        new RowDefinition { Height = GridLength.Auto }, // Code
                        new RowDefinition { Height = GridLength.Auto }  // Code
                    }
                };

                var nameLabel = new Label { Text = "Name" };
                nameLabel.SetBinding(Label.TextProperty, new Binding("Name", BindingMode.Default, null, null, "{0} (Type: ShaderLibrary)"));

                var codeLabel = new Label { Text = "Code:" };
                var codeEntry = TemplateBuilder.CreateReadOnlyEditor("Code");

                TemplateBuilder.AddGridRow(grid, nameLabel, 0, GridLength.Auto);
                TemplateBuilder.AddGridRow(grid, codeLabel, 1, GridLength.Auto);
                TemplateBuilder.AddGridRow(grid, codeEntry, 2, GridLength.Auto);

                return grid;
            });

            AssetFileShader = new DataTemplate(() =>
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

                var nameLabel = new Label { Text = "Name" };
                nameLabel.SetBinding(Label.TextProperty, new Binding("Name", BindingMode.Default, null, null, "{0} (Type: Shader)"));

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
            });

            AssetFileTexture = new DataTemplate(() =>
            {
                var grid = new Grid
                {
                    RowDefinitions =
                    {
                        new RowDefinition { Height = GridLength.Auto },
                        new RowDefinition { Height = GridLength.Auto },
                        new RowDefinition { Height = GridLength.Auto },
                        new RowDefinition { Height = GridLength.Auto },
                        new RowDefinition { Height = GridLength.Auto },
                        new RowDefinition { Height = GridLength.Auto },
                        new RowDefinition { Height = GridLength.Auto }
                    },
                    ColumnDefinitions =
                    {
                        new ColumnDefinition { Width = GridLength.Auto },
                        new ColumnDefinition { Width = GridLength.Star }
                    }
                };

                var nameLabel = new Label { Text = "Name" };
                nameLabel.SetBinding(Label.TextProperty, new Binding("Name", BindingMode.Default, null, null, "{0} (Type: Texture)"));

                var image = new Image
                {
                    Aspect = Aspect.AspectFit,
                    HorizontalOptions = LayoutOptions.Start,
                    VerticalOptions = LayoutOptions.Start
                };
                image.SetBinding(Image.SourceProperty, new Binding("Thumbnail"));

                var generateMipsCheckBox = TemplateBuilder.CreateCheckBox("ShouldGenerateMips");
                var storageBindingCheckBox = TemplateBuilder.CreateCheckBox("ShouldSupportStorageBinding");
                var filtrationPicker = TemplateBuilder.CreateEnumPicker<TextureFiltration>("Filtration");
                var clampingPicker = TemplateBuilder.CreateEnumPicker<TextureClamping>("Clamping");
                var formatPicker = TemplateBuilder.CreateEnumPicker<TextureFormat>("Format");

                TemplateBuilder.AddGridRow(grid, nameLabel, 0, GridLength.Auto);
                TemplateBuilder.AddGridRow(grid, image, 1, GridLength.Auto);
                TemplateBuilder.AddGridRowWithLabel(grid, "Generate Mips:", generateMipsCheckBox, 2);
                TemplateBuilder.AddGridRowWithLabel(grid, "Storage Binding:", storageBindingCheckBox, 3);
                TemplateBuilder.AddGridRowWithLabel(grid, "Filtration:", filtrationPicker, 4);
                TemplateBuilder.AddGridRowWithLabel(grid, "Clamping:", clampingPicker, 5);
                TemplateBuilder.AddGridRowWithLabel(grid, "Format:", formatPicker, 6);

                return grid;
            });
        }
        protected override DataTemplate OnSelectTemplate(object item, BindableObject container)
        {
            if (item is TextureFile)
                return AssetFileTexture;

            if (item is ShaderFile)
                return AssetFileShader;

            if (item is ShaderLibraryFile)
                return AssetFileShaderLibrary;

            return Default;
        }
    }
}