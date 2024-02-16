using SailorEditor.Helpers;

public class ShaderLibraryFileTemplate : DataTemplate
{
    public ShaderLibraryFileTemplate()
    {
        LoadTemplate = () =>
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
        };
    }
}