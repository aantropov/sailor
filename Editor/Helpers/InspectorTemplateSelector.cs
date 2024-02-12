using SailorEditor.ViewModels;
using SailorEditor.Services;
using Microsoft.Maui.Controls;
using System.ComponentModel;
using SailorEditor.Engine;
using System.Globalization;
using System.Xml;

namespace SailorEditor.Helpers
{
    public class AssetUIDToFilenameConverter : IValueConverter
    {
        public object Convert(object value, Type targetType, object parameter, CultureInfo culture)
        {
            var assetUID = value as string;
            if (string.IsNullOrEmpty(assetUID))
                return null;

            var AssetService = MauiProgram.GetService<AssetsService>();
            return AssetService.Assets[assetUID].DisplayName;
        }

        public object ConvertBack(object value, Type targetType, object parameter, CultureInfo culture)
        {
            throw new NotImplementedException();
        }
    }

    public class DisplayNameBehavior : Behavior<Label>
    {
        protected override void OnAttachedTo(Label bindable)
        {
            base.OnAttachedTo(bindable);
            bindable.BindingContextChanged += OnBindingContextChanged;
        }
        protected override void OnDetachingFrom(Label bindable)
        {
            base.OnDetachingFrom(bindable);
            bindable.BindingContextChanged -= OnBindingContextChanged;
        }
        private void OnBindingContextChanged(object sender, EventArgs e)
        {
            var label = sender as Label;
            if (label != null && label.BindingContext != null)
            {
                var bindingContext = label.BindingContext;
                if (bindingContext is INotifyPropertyChanged notifyContext)
                {
                    notifyContext.PropertyChanged += (s, args) =>
                    {
                        if (args.PropertyName == "DisplayName" || args.PropertyName == "IsDirty")
                        {
                            UpdateLabel(label, bindingContext);
                        }
                    };
                    UpdateLabel(label, bindingContext);
                }
            }
        }

        private void UpdateLabel(Label label, object bindingContext)
        {
            var model = bindingContext as AssetFile;

            if (model != null)
            {
                label.Text = model.IsDirty ? $"*{model.DisplayName}*" : $"{model.DisplayName}";
                label.Text += $" (Type: {model.GetType().Name})";
            }
        }
    }

    public class InspectorTemplateSelector : DataTemplateSelector
    {
        public DataTemplate Default { get; private set; }
        public DataTemplate AssetFileTexture { get; private set; }
        public DataTemplate AssetFileModel { get; private set; }
        public DataTemplate AssetFileShader { get; private set; }
        public DataTemplate AssetFileShaderLibrary { get; private set; }

        public InspectorTemplateSelector()
        {
            Default = new DataTemplate(() =>
            {
                var label = new Label();
                label.SetBinding(Label.TextProperty, new Binding("DisplayName"));
                label.Behaviors.Add(new DisplayNameBehavior());
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

                var nameLabel = new Label { Text = "DisplayName" };
                nameLabel.SetBinding(Label.TextProperty, new Binding("DisplayName"));
                nameLabel.Behaviors.Add(new DisplayNameBehavior());

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

                var nameLabel = new Label { Text = "DisplayName" };
                nameLabel.SetBinding(Label.TextProperty, new Binding("DisplayName"));
                nameLabel.Behaviors.Add(new DisplayNameBehavior());

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

            AssetFileModel = new DataTemplate(() =>
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
                        new RowDefinition { Height = GridLength.Auto }
                    },

                    ColumnDefinitions =
                    {
                        new ColumnDefinition { Width = GridLength.Auto },
                        new ColumnDefinition { Width = GridLength.Star }
                    }
                };

                var nameLabel = new Label { Text = "DisplayName" };
                nameLabel.SetBinding(Label.TextProperty, new Binding("DisplayName"));
                nameLabel.Behaviors.Add(new DisplayNameBehavior());

                var saveButton = new Button
                {
                    Text = "Save",
                    HorizontalOptions = LayoutOptions.Start,
                    VerticalOptions = LayoutOptions.Start
                };
                saveButton.SetBinding(Button.IsVisibleProperty, new Binding("IsDirty"));
                saveButton.Clicked += (sender, e) => (saveButton.BindingContext as AssetFile).UpdateAssetFile();

                var revertButton = new Button
                {
                    Text = "Revert",
                    HorizontalOptions = LayoutOptions.End,
                    VerticalOptions = LayoutOptions.Start
                };
                revertButton.SetBinding(Button.IsVisibleProperty, new Binding("IsDirty"));
                revertButton.Clicked += (sender, e) => (revertButton.BindingContext as AssetFile).Revert();

                var bShouldGenerateMaterials = TemplateBuilder.CreateCheckBox("ShouldGenerateMaterials");
                var bShouldBatchByMaterial = TemplateBuilder.CreateCheckBox("ShouldBatchByMaterial");

                TemplateBuilder.AddGridRow(grid, nameLabel, 0, GridLength.Auto);
                TemplateBuilder.AddGridRow(grid, saveButton, 1, GridLength.Auto);
                TemplateBuilder.AddGridRow(grid, revertButton, 1, GridLength.Auto);

                Grid.SetColumn(saveButton, 0);
                Grid.SetColumn(revertButton, 1);

                TemplateBuilder.AddGridRowWithLabel(grid, "Generate Materials:", bShouldGenerateMaterials, 2);
                TemplateBuilder.AddGridRowWithLabel(grid, "Batch by material:", bShouldBatchByMaterial, 3);

                var materialsHeaderLabel = new Label
                {
                    Text = "Default Materials",
                    FontAttributes = FontAttributes.Bold,
                    HorizontalOptions = LayoutOptions.Start
                };
                TemplateBuilder.AddGridRow(grid, materialsHeaderLabel, 4, GridLength.Auto);

                var materialsCollectionView = new CollectionView
                {
                    ItemTemplate = new DataTemplate(() =>
                    {
                        var fileNameLabel = new Label();
                        fileNameLabel.SetBinding(Label.TextProperty,
                            new Binding(".", BindingMode.Default, new AssetUIDToFilenameConverter()));
                        return fileNameLabel;
                    }),
                    SelectionMode = SelectionMode.Single
                };
                
                materialsCollectionView.SetBinding(ItemsView.ItemsSourceProperty, new Binding("DefaultMaterials"));
                materialsCollectionView.SelectionChanged += (sender, e) =>
                {
                    var selectedMaterial = e.CurrentSelection.FirstOrDefault() as string;
                    if (selectedMaterial != null)
                    {
                        var assetFile = MauiProgram.GetService<AssetsService>().Assets[selectedMaterial];
                        MauiProgram.GetService<SelectionService>().OnSelectAsset(assetFile);
                    }

                    materialsCollectionView.SelectedItem = null;
                };

                TemplateBuilder.AddGridRow(grid, materialsCollectionView, 5, GridLength.Auto);

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

                var nameLabel = new Label { Text = "DisplayName" };
                nameLabel.SetBinding(Label.TextProperty, new Binding("DisplayName"));
                nameLabel.Behaviors.Add(new DisplayNameBehavior());

                var saveButton = new Button
                {
                    Text = "Save",
                    HorizontalOptions = LayoutOptions.Start,
                    VerticalOptions = LayoutOptions.Start
                };
                saveButton.SetBinding(Button.IsVisibleProperty, new Binding("IsDirty"));
                saveButton.Clicked += (sender, e) => (saveButton.BindingContext as AssetFile).UpdateAssetFile();

                var revertButton = new Button
                {
                    Text = "Revert",
                    HorizontalOptions = LayoutOptions.End,
                    VerticalOptions = LayoutOptions.Start
                };
                revertButton.SetBinding(Button.IsVisibleProperty, new Binding("IsDirty"));
                revertButton.Clicked += (sender, e) => (revertButton.BindingContext as AssetFile).Revert();

                var image = new Image
                {
                    WidthRequest = 256,
                    HeightRequest = 256,
                    Aspect = Aspect.AspectFit,
                    HorizontalOptions = LayoutOptions.Start,
                    VerticalOptions = LayoutOptions.Start
                };
                image.SetBinding(Image.SourceProperty, new Binding("Texture"));

                var generateMipsCheckBox = TemplateBuilder.CreateCheckBox("ShouldGenerateMips");
                var storageBindingCheckBox = TemplateBuilder.CreateCheckBox("ShouldSupportStorageBinding");
                var filtrationPicker = TemplateBuilder.CreateEnumPicker<TextureFiltration>("Filtration");
                var clampingPicker = TemplateBuilder.CreateEnumPicker<TextureClamping>("Clamping");
                var formatPicker = TemplateBuilder.CreateEnumPicker<TextureFormat>("Format");

                TemplateBuilder.AddGridRow(grid, nameLabel, 0, GridLength.Auto);
                TemplateBuilder.AddGridRow(grid, saveButton, 1, GridLength.Auto);
                TemplateBuilder.AddGridRow(grid, revertButton, 1, GridLength.Auto);

                Grid.SetColumn(saveButton, 0);
                Grid.SetColumn(revertButton, 1);

                TemplateBuilder.AddGridRow(grid, image, 2, GridLength.Auto);
                TemplateBuilder.AddGridRowWithLabel(grid, "Generate Mips:", generateMipsCheckBox, 3);
                TemplateBuilder.AddGridRowWithLabel(grid, "Storage Binding:", storageBindingCheckBox, 4);
                TemplateBuilder.AddGridRowWithLabel(grid, "Filtration:", filtrationPicker, 5);
                TemplateBuilder.AddGridRowWithLabel(grid, "Clamping:", clampingPicker, 6);
                TemplateBuilder.AddGridRowWithLabel(grid, "Format:", formatPicker, 7);

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

            if (item is ModelFile)
                return AssetFileModel;

            return Default;
        }
    }
}