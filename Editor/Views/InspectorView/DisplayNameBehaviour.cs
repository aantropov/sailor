using SailorEditor.ViewModels;
using System.ComponentModel;

public class DisplayNameBehavior : Behavior<Label>
{
    protected override void OnAttachedTo(Label bindable)
    {
        base.OnAttachedTo(bindable);
        bindable.BindingContextChanged += OnBindingContextChanged;
    }
    protected override void OnDetachingFrom(Label bindable)
    {
        base.OnDetachingFrom(bindable);
        bindable.BindingContextChanged -= OnBindingContextChanged;
    }
    private void OnBindingContextChanged(object sender, EventArgs e)
    {
        var label = sender as Label;
        if (label != null && label.BindingContext != null)
        {
            var bindingContext = label.BindingContext;
            if (bindingContext is INotifyPropertyChanged notifyContext)
            {
                notifyContext.PropertyChanged += (s, args) =>
                {
                    if (args.PropertyName == "DisplayName" || args.PropertyName == "IsDirty")
                    {
                        UpdateLabel(label, bindingContext);
                    }
                };
                UpdateLabel(label, bindingContext);
            }
        }
    }

    private void UpdateLabel(Label label, object bindingContext)
    {
        var model = bindingContext as AssetFile;

        if (model != null)
        {
            label.Text = model.IsDirty ? $"*{model.DisplayName}*" : $"{model.DisplayName}";
            label.Text += $" (Type: {model.GetType().Name})";
        }
    }
}