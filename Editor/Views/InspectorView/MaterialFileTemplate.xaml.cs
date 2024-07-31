namespace SailorEditor;

[XamlCompilation(XamlCompilationOptions.Skip)]
public partial class MaterialFileTemplate : DataTemplate
{
    public MaterialFileTemplate()
    {
        InitializeComponent();
    }

    void OnEntryCompleted(object sender, EventArgs e) => ((Entry)sender).Unfocus();
}