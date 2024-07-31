namespace SailorEditor;

[XamlCompilation(XamlCompilationOptions.Skip)]
public partial class GameObjectTemplate : DataTemplate
{
    public GameObjectTemplate()
    {
        InitializeComponent();
    }

    void OnEntryCompleted(object sender, EventArgs e) => ((Entry)sender).Unfocus();
}