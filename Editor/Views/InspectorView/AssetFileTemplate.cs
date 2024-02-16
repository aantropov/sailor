using SailorEditor.Helpers;

public class AssetFileTemplate : DataTemplate
{
    public AssetFileTemplate()
    {
        LoadTemplate = () =>
        {
            var label = new Label();
            label.SetBinding(Label.TextProperty, new Binding("DisplayName"));
            label.Behaviors.Add(new DisplayNameBehavior());
            label.TextColor = Color.FromRgb(255, 0, 0);

            return label;
        };
    }
}