using SailorEditor;
using SailorEditor.Helpers;
using SailorEditor.Services;
using SailorEditor.Utility;
using SailorEditor.ViewModels;

public class ModelFileTemplate : AssetFileTemplate
{
    public ModelFileTemplate()
    {
        LoadTemplate = () =>
        {
            var props = new Grid { ColumnDefinitions = { new ColumnDefinition { Width = GridLength.Auto }, new ColumnDefinition { Width = GridLength.Star } } };
            TemplateBuilder.AddGridRowWithLabel(props, "Generate materials", TemplateBuilder.CreateCheckBox(nameof(ModelFile.ShouldGenerateMaterials)), GridLength.Auto);
            TemplateBuilder.AddGridRowWithLabel(props, "Batch by material", TemplateBuilder.CreateCheckBox(nameof(ModelFile.ShouldBatchByMaterial)), GridLength.Auto);

            /*
            var materialsCollectionView = new CollectionView
            {
                ItemTemplate = new DataTemplate(() =>
                {
                    var fileNameLabel = new Label();
                    fileNameLabel.SetBinding(Label.TextProperty, new Binding("Value", BindingMode.Default, new AssetUIDToFilenameConverter()));
                    return fileNameLabel;
                }),
                SelectionMode = SelectionMode.Single
            };

            materialsCollectionView.SetBinding(ItemsView.ItemsSourceProperty, new Binding(nameof(ModelFile.DefaultMaterials)));
            materialsCollectionView.SelectionChanged += (sender, e) =>
            {
                if (e.CurrentSelection.FirstOrDefault() is string selectedMaterial)
                    MauiProgram.GetService<SelectionService>().OnSelectAsset(MauiProgram.GetService<AssetsService>().Assets[selectedMaterial]);

                materialsCollectionView.SelectedItem = null;
            };*/


            var grid = new Grid { ColumnDefinitions = { new ColumnDefinition { Width = GridLength.Auto } } };
            TemplateBuilder.AddGridRow(grid, CreateControlPanel(), GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, new Label { Text = "Properties", FontAttributes = FontAttributes.Bold }, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, props, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, TemplateBuilder.CreateListEditor<Observable<string>>(nameof(ModelFile.DefaultMaterials), "Default Materials", "", new AssetUIDToFilenameConverter()), GridLength.Auto);

            return grid;
        };
    }
}