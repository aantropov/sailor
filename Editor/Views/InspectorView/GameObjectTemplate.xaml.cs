using SailorEditor.ViewModels;
using SailorEditor.Services;

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

    async void OnAddComponentClicked(object sender, EventArgs e)
    {
        if (ResolveGameObject(sender) is GameObject gameObject)
            await gameObject.AddComponentFromInspectorAsync();
    }

    void OnClearComponentsClicked(object sender, EventArgs e)
    {
        if (ResolveGameObject(sender) is GameObject gameObject)
            gameObject.ClearComponentsFromInspector();
    }

    static GameObject ResolveGameObject(object sender)
    {
        if (sender is BindableObject { BindingContext: GameObject direct })
            return direct;

        if (sender is Element element)
        {
            for (var current = element.Parent; current is not null; current = current.Parent)
            {
                if (current.BindingContext is GameObject gameObject)
                    return gameObject;
            }
        }

        return MauiProgram.GetService<SelectionService>().SelectedItem as GameObject;
    }
}
