using SailorEditor.Helpers;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEngine;

public class ModelFileTemplate : AssetFileTemplate
{
    public ModelFileTemplate()
    {
        LoadTemplate = () =>
        {
            var props = new Grid { ColumnDefinitions = { new ColumnDefinition { Width = GridLength.Auto }, new ColumnDefinition { Width = GridLength.Star } } };
            Templates.AddGridRowWithLabel(props, "Generate materials", Templates.CheckBox(static (ModelFile vm) => vm.ShouldGenerateMaterials, static (ModelFile vm, bool value) => vm.ShouldGenerateMaterials = value), GridLength.Auto);
            Templates.AddGridRowWithLabel(props, "Batch by material", Templates.CheckBox(static (ModelFile vm) => vm.ShouldBatchByMaterial, static (ModelFile vm, bool value) => vm.ShouldBatchByMaterial = value), GridLength.Auto);
            Templates.AddGridRowWithLabel(props, "Unit Scale",
                Templates.EntryField(static (ModelFile vm) => vm.UnitScale, static (ModelFile vm, float value) => vm.UnitScale = value, new FloatValueConverter()), GridLength.Auto);

            var grid = new Grid { ColumnDefinitions = { new ColumnDefinition { Width = GridLength.Auto } } };
            Templates.AddGridRow(grid, CreateControlPanel(), GridLength.Auto);
            Templates.AddGridRow(grid, new Label { Text = "Properties", FontAttributes = FontAttributes.Bold }, GridLength.Auto);
            Templates.AddGridRow(grid, props, GridLength.Auto);
            Templates.AddGridRow(grid, Templates.ListEditor(
                 static (ModelFile vm) => vm.DefaultMaterials,
                 static (ModelFile vm, ObservableList<Observable<FileId>> value) => vm.DefaultMaterials = value,
                 () => Templates.FileIdEditor(
                            static (Observable<FileId> vm) => vm.Value,
                            static (Observable<FileId> vm, FileId value) => vm.Value = value),
                "Default Materials",
                new FileId(),
                converter: new FileIdToFilenameConverter()), GridLength.Auto);

            return grid;
        };
    }
}