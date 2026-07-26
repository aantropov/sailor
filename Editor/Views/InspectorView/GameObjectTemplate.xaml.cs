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
        ((Entry)sender).Unfocus();
    }

    void OnEntryUnfocused(object sender, FocusEventArgs e)
    {
        if (sender is not Entry { BindingContext: IInspectorEditable editable } entry)
            return;

        entry.Dispatcher.DispatchDelayed(TimeSpan.FromMilliseconds(1), () =>
        {
            if (!editable.HasPendingInspectorChanges)
                return;

            try
            {
                editable.CommitInspectorChanges();
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"Inspector commit failed: {ex}");
            }
        });
    }

    async void OnAddComponentClicked(object sender, EventArgs e)
    {
        if (ResolveGameObject(sender) is GameObject gameObject)
            await gameObject.AddComponentFromInspectorAsync();
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
