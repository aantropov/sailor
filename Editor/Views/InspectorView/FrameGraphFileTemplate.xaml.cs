namespace SailorEditor;

[XamlCompilation(XamlCompilationOptions.Compile)]
public partial class FrameGraphFileTemplate : DataTemplate
{
    public FrameGraphFileTemplate()
    {
        InitializeComponent();
    }

    private void OnEntryCompleted(object sender, EventArgs args)
    {
        ((Entry)sender).Unfocus();
    }
}
