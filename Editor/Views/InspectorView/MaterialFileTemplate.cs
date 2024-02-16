using SailorEditor.Engine;
using SailorEditor.Helpers;
using SailorEditor.ViewModels;

public class MaterialFileTemplate : DataTemplate
{
    public MaterialFileTemplate()
    {
        LoadTemplate = () =>
        {
            var grid = new Grid
            {
                RowDefinitions =
                    {
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

            var renderQueueEntry = TemplateBuilder.CreateEntryWithLabel("RenderQueue", "Render Queue");
            var depthBiasEntry = TemplateBuilder.CreateEntryWithLabel("DepthBias", "Depth Bias");

            var enableDepthTestCheckBox = TemplateBuilder.CreateCheckBox("EnableDepthTest");
            var enableZWriteCheckBox = TemplateBuilder.CreateCheckBox("EnableZWrite");
            var customDepthShaderCheckBox = TemplateBuilder.CreateCheckBox("CustomDepthShader");
            var supportMultisamplingCheckBox = TemplateBuilder.CreateCheckBox("SupportMultisampling");

            var uniformsFloatEditor = TemplateBuilder.CreateDictionaryEditor("UniformsFloat", "Uniforms Float");
            var uniformsVec4Editor = TemplateBuilder.CreateDictionaryEditor("UniformsVec4", "Uniforms Vec4");
            var samplersEditor = TemplateBuilder.CreateDictionaryEditor("Samplers", "Samplers");

            var shaderLabel = new Label();
            shaderLabel.SetBinding(Label.TextProperty, new Binding("Shader", BindingMode.Default, new AssetUIDToFilenameConverter()));

            var fillModePicker = TemplateBuilder.CreateEnumPicker<FillMode>("FillMode");
            var blendModePicker = TemplateBuilder.CreateEnumPicker<SailorEditor.Engine.BlendMode>("BlendMode");
            var cullModePicker = TemplateBuilder.CreateEnumPicker<CullMode>("CullMode");

            var definesEditor = TemplateBuilder.CreateListEditor("Defines", "Defines");

            TemplateBuilder.AddGridRow(grid, nameLabel, 0, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, saveButton, 1, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, revertButton, 1, GridLength.Auto);

            Grid.SetColumn(saveButton, 0);
            Grid.SetColumn(revertButton, 1);

            TemplateBuilder.AddGridRow(grid, renderQueueEntry, 2, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, depthBiasEntry, 3, GridLength.Auto);
            TemplateBuilder.AddGridRowWithLabel(grid, "Enable DepthTest", enableDepthTestCheckBox, 4);
            TemplateBuilder.AddGridRowWithLabel(grid, "enableDepthTestCheckBox", enableZWriteCheckBox, 5);
            TemplateBuilder.AddGridRowWithLabel(grid, "enableDepthTestCheckBox", customDepthShaderCheckBox, 6);
            TemplateBuilder.AddGridRowWithLabel(grid, "enableDepthTestCheckBox", supportMultisamplingCheckBox, 7);
            TemplateBuilder.AddGridRow(grid, uniformsFloatEditor, 8, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, uniformsVec4Editor, 9, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, samplersEditor, 10, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, shaderLabel, 11, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, fillModePicker, 12, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, blendModePicker, 13, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, cullModePicker, 14, GridLength.Auto);
            TemplateBuilder.AddGridRow(grid, definesEditor, 15, GridLength.Auto);

            return grid;
        };
    }
}