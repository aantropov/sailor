using SailorEditor.Services;
using SailorEditor.Utility;
using SailorEditor.ViewModels;
using SailorEngine;

namespace SailorEditor.Controls;

public class InstanceIdDragAndDropBehaviour : Behavior<Label>
{
    private DragGestureRecognizer _dragGestureRecognizer;
    private DropGestureRecognizer _dropGestureRecognizer;
    private Label _associatedLabel;

    public static readonly BindableProperty BoundPropertyProperty =
        BindableProperty.Create(nameof(BoundProperty), typeof(object), typeof(InstanceIdDragAndDropBehaviour), null, BindingMode.TwoWay);

    public object BoundProperty
    {
        get => GetValue(BoundPropertyProperty);
        set => SetValue(BoundPropertyProperty, value);
    }

    public static readonly BindableProperty ExpectedTypenameProperty =
        BindableProperty.Create(nameof(ExpectedTypename), typeof(string), typeof(InstanceIdDragAndDropBehaviour), string.Empty);

    public string ExpectedTypename
    {
        get => (string)GetValue(ExpectedTypenameProperty);
        set => SetValue(ExpectedTypenameProperty, value);
    }

    protected override void OnAttachedTo(Label bindable)
    {
        base.OnAttachedTo(bindable);

        _associatedLabel = bindable;
        _associatedLabel.BindingContextChanged += OnBindingContextChanged;
        BindingContext = _associatedLabel.BindingContext;

        _dragGestureRecognizer = new DragGestureRecognizer();
        _dragGestureRecognizer.DragStarting += OnDragStarting;

        _dropGestureRecognizer = new DropGestureRecognizer();
        _dropGestureRecognizer.Drop += OnDrop;

        bindable.GestureRecognizers.Add(_dragGestureRecognizer);
        bindable.GestureRecognizers.Add(_dropGestureRecognizer);
    }

    protected override void OnDetachingFrom(Label bindable)
    {
        base.OnDetachingFrom(bindable);

        _associatedLabel.BindingContextChanged -= OnBindingContextChanged;
        _associatedLabel = null;
    }

    private void OnBindingContextChanged(object sender, EventArgs e)
    {
        BindingContext = _associatedLabel.BindingContext;
    }

    private void OnDragStarting(object sender, DragStartingEventArgs e)
    {
        if (BoundProperty != null)
        {
            e.Data.Properties[EditorDragDrop.DragItemKey] = BoundProperty;
        }
    }

    private void OnDrop(object sender, DropEventArgs e)
    {
        if (!e.Data.Properties.TryGetValue(EditorDragDrop.DragItemKey, out var droppedItem))
        {
            return;
        }

        if (droppedItem is Component component)
        {
            if (MatchesExpectedType(component.Typename?.Name))
            {
                BoundProperty = component.InstanceId;
                e.Handled = true;
            }
            return;
        }

        if (droppedItem is not GameObject gameObject)
        {
            return;
        }

        var instanceId = ResolveInstanceId(gameObject);
        if (instanceId != null)
        {
            BoundProperty = instanceId;
            e.Handled = true;
        }
    }

    private InstanceId ResolveInstanceId(GameObject gameObject)
    {
        if (string.IsNullOrWhiteSpace(ExpectedTypename) || ExpectedTypename.EndsWith("GameObject", StringComparison.Ordinal))
        {
            return gameObject.InstanceId;
        }

        var component = MauiProgram.GetService<WorldService>().FindComponent(gameObject, ExpectedTypename);
        return component?.InstanceId;
    }

    private bool MatchesExpectedType(string typename)
    {
        return string.IsNullOrWhiteSpace(ExpectedTypename) ||
            string.Equals(ExpectedTypename, typename, StringComparison.Ordinal) ||
            ExpectedTypename.EndsWith("::" + typename?.Split("::").LastOrDefault(), StringComparison.Ordinal);
    }
}
