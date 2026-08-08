using SailorEditor.Services;
using SailorEditor.ViewModels;
using SailorEditor;
using SailorEngine;
using SailorEditor.Controls;

public class InstanceIdSelectBehavior : Behavior<Label>
{
    private Label _associatedLabel;
    private TapGestureRecognizer _tapGestureRecognizer;

    public static readonly BindableProperty BoundPropertyProperty =
        BindableProperty.Create(nameof(BoundProperty), typeof(object), typeof(InstanceIdSelectBehavior), null, BindingMode.TwoWay);

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

        _tapGestureRecognizer = new TapGestureRecognizer();
        _tapGestureRecognizer.Tapped += OnLabelTapped;
        bindable.GestureRecognizers.Add(_tapGestureRecognizer);
    }

    protected override void OnDetachingFrom(Label bindable)
    {
        base.OnDetachingFrom(bindable);

        if (_tapGestureRecognizer is not null)
        {
            _tapGestureRecognizer.Tapped -= OnLabelTapped;
            bindable.GestureRecognizers.Remove(_tapGestureRecognizer);
            _tapGestureRecognizer = null;
        }
        _associatedLabel.BindingContextChanged -= OnBindingContextChanged;
        _associatedLabel = null;
    }
    private void OnBindingContextChanged(object sender, System.EventArgs e)
    {
        var bindingContext = _associatedLabel.BindingContext;
        BindingContext = _associatedLabel.BindingContext;
    }

    private async void OnLabelTapped(object sender, EventArgs e)
    {
        try
        {
            await MauiProgram.GetService<SelectionService>()
                .NavigateToReferenceAsync(BoundProperty);
        }
        catch (Exception exception)
        {
            Console.WriteLine(
                $"[InstanceIdSelectBehavior] Failed to navigate to reference: {exception.Message}");
        }
    }
}
