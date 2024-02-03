using SailorEditor.ViewModels;
using SailorEditor.Services;
using Microsoft.Maui.Controls;
using System.ComponentModel;

namespace SailorEditor.Helpers
{
    public class InspectorTemplateSelector : DataTemplateSelector
    {
        Editor CreateReadOnlyEditor(string bindingPath)
        {
            var editor = new Editor
            {
                FontSize = 12,
                IsReadOnly = true,
                AutoSize = EditorAutoSizeOption.TextChanges
            };

            editor.SetBinding(Editor.TextProperty, new Binding(bindingPath));
            return editor;
        }

        View CreateShaderCodeView(string title, string bindingPath)
        {
            var grid = new Grid { RowDefinitions = { new RowDefinition { Height = GridLength.Auto }, new RowDefinition { Height = GridLength.Auto } } };
            var titleLabel = new Label
            {
                Text = title,
                VerticalOptions = LayoutOptions.Center
            };

            var codeView = CreateReadOnlyEditor(bindingPath);

            var stackLayout = new VerticalStackLayout();
            stackLayout.Children.Add(titleLabel);
            stackLayout.Children.Add(codeView);

            return stackLayout;
        }
        void AddGridRow(Grid grid, View view, int rowIndex, GridLength gridLength)
        {
            if (grid.RowDefinitions.Count <= rowIndex)
            {
                grid.RowDefinitions.Add(new RowDefinition { Height = gridLength });
            }
            grid.Children.Add(view);
            Grid.SetRow(view, rowIndex);
        }

        public DataTemplate Default { get; set; }
        public DataTemplate AssetFileTexture { get; set; }
        public DataTemplate AssetFileShader { get; set; }

        public InspectorTemplateSelector()
        {
            Default = new DataTemplate(() =>
            {
                var label = new Label();
                label.SetBinding(Label.TextProperty, new Binding("Name"));
                label.TextColor = Color.FromRgb(255, 0, 0);
                return label;
            });

            AssetFileShader = new DataTemplate(() =>
            {
                var grid = new Grid
                {
                    RowDefinitions =
                    {
                        new RowDefinition { Height = GridLength.Auto }, // Name
                        new RowDefinition { Height = GridLength.Auto }, // Includes
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
                var includesList = CreateReadOnlyEditor("Includes");
                var definesLabel = new Label { Text = "Defines:" };
                var definesList = CreateReadOnlyEditor("Defines");

                var commonShaderExpander = CreateShaderCodeView("Common Shader", "GlslCommonShader");
                var vertexShaderExpander = CreateShaderCodeView("Vertex Shader", "GlslVertexShader");
                var fragmentShaderExpander = CreateShaderCodeView("Fragment Shader", "GlslFragmentShader");
                var computeShaderExpander = CreateShaderCodeView("Compute Shader", "GlslComputeShader");

                AddGridRow(grid, nameLabel, 0, GridLength.Auto);
                AddGridRow(grid, includesLabel, 1, GridLength.Auto);
                AddGridRow(grid, includesList, 2, GridLength.Auto);
                AddGridRow(grid, definesLabel, 3, GridLength.Auto);
                AddGridRow(grid, definesList, 4, GridLength.Auto);
                AddGridRow(grid, commonShaderExpander, 5, GridLength.Auto);
                AddGridRow(grid, vertexShaderExpander, 6, GridLength.Auto);
                AddGridRow(grid, fragmentShaderExpander, 7, GridLength.Auto);
                AddGridRow(grid, computeShaderExpander, 8, GridLength.Auto);

                return grid;
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
            if (item is TextureFile)
                return AssetFileTexture;

            if (item is ShaderFile)
                return AssetFileShader;

            return Default;
        }
    }
}