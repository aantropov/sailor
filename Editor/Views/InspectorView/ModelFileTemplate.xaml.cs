namespace SailorEditor;

[XamlCompilation(XamlCompilationOptions.Skip)]
public partial class ModelFileTemplate : DataTemplate
{
    public ModelFileTemplate()
    {
        InitializeComponent();
    }

    void OnEntryCompleted(object sender, EventArgs e) => ((Entry)sender).Unfocus();
}