using SailorEditor.ViewModels;

namespace SailorEditor;

[XamlCompilation(XamlCompilationOptions.Skip)]
public partial class GameObjectTemplate : DataTemplate
{
    public GameObjectTemplate()
    {
        InitializeComponent();
    }

    void OnEntryCompleted(object sender, EventArgs e)
    {
        CommitFrom(sender);
        ((Entry)sender).Unfocus();
    }

    void OnEntryUnfocused(object sender, FocusEventArgs e) => CommitFrom(sender);

    static void CommitFrom(object sender)
    {
        if (sender is Entry { BindingContext: IInspectorEditable editable } && editable.HasPendingInspectorChanges)
            editable.CommitInspectorChanges();
    }
}
