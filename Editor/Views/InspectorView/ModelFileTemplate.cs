using SailorEditor;
using SailorEditor.Helpers;
using SailorEditor.Services;
using SailorEditor.ViewModels;

public class ModelFileTemplate : DataTemplate
{
    public ModelFileTemplate()
    {
        LoadTemplate = () =>
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
        };
    }
}