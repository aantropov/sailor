using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEditor;
using SailorEngine;
using SailorEditor.Controls;

public class InstanceIdSelectBehavior : Behavior<Label>
{
    private Label _associatedLabel;

    public static readonly BindableProperty BoundPropertyProperty =
        BindableProperty.Create(nameof(BoundProperty), typeof(object), typeof(FileIdDragAndDropBehaviour), null, BindingMode.TwoWay);

    public object BoundProperty
    {
        get => GetValue(BoundPropertyProperty);
        set => SetValue(BoundPropertyProperty, value);
    }

    protected override void OnAttachedTo(Label bindable)
    {
        base.OnAttachedTo(bindable);

        _associatedLabel = bindable;
        _associatedLabel.BindingContextChanged += OnBindingContextChanged;

        BindingContext = _associatedLabel.BindingContext;

        var tapGestureRecognizer = new TapGestureRecognizer();
        tapGestureRecognizer.Tapped += OnLabelTapped;
        bindable.GestureRecognizers.Add(tapGestureRecognizer);
    }

    protected override void OnDetachingFrom(Label bindable)
    {
        base.OnDetachingFrom(bindable);

        _associatedLabel.BindingContextChanged -= OnBindingContextChanged;
        _associatedLabel = null;
    }
    private void OnBindingContextChanged(object sender, System.EventArgs e)
    {
        var bindingContext = _associatedLabel.BindingContext;
        BindingContext = _associatedLabel.BindingContext;
    }

    private void OnLabelTapped(object sender, EventArgs e)
    {
        if (BoundProperty != null)
        {
            if (BoundProperty is InstanceId id)
            {
                MauiProgram.GetService<SelectionService>().SelectInstance(id);
            }
        }
    }
}